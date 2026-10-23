#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "GameFramework/InputSettings.h"
#include "Engine/DataTable.h"
#include "UObject/StructOnScope.h"
#include "JsonObjectConverter.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/DataTableFactory.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

FUnrealMCPProjectCommands::FUnrealMCPProjectCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("create_input_mapping"))
    {
        return HandleCreateInputMapping(Params);
    }
    else if (CommandType == TEXT("create_data_table"))
    {
        return HandleCreateDataTable(Params);
    }
    else if (CommandType == TEXT("add_row"))
    {
        return HandleAddRow(Params);
    }
    else if (CommandType == TEXT("read_row"))
    {
        return HandleReadRow(Params);
    }
    else if (CommandType == TEXT("set_asset_property"))
    {
        return HandleSetAssetProperty(Params);
    }
    else if (CommandType == TEXT("refresh_project_reflection"))
    {
        return HandleRefreshProjectReflection(Params);
    }
    else if (CommandType == TEXT("compile_project"))
    {
        return HandleCompileProject(Params);
    }
    else if (CommandType == TEXT("save_assets"))
    {
        return HandleSaveAssets(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown project command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    FString Key;
    if (!Params->TryGetStringField(TEXT("key"), Key))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'key' parameter"));
    }

    // Get the input settings
    UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
    if (!InputSettings)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
    }

    // 客户端用 input_type 区分 Action 与 Axis；不能静默按 Action 处理 Axis 请求。
    FString InputType = TEXT("Action");
    Params->TryGetStringField(TEXT("input_type"), InputType);
    const bool bAxisMapping = InputType.Equals(TEXT("Axis"), ESearchCase::IgnoreCase);

    bool bShift = false;
    bool bCtrl = false;
    bool bAlt = false;
    bool bCmd = false;
    Params->TryGetBoolField(TEXT("shift"), bShift);
    Params->TryGetBoolField(TEXT("ctrl"), bCtrl);
    Params->TryGetBoolField(TEXT("alt"), bAlt);
    Params->TryGetBoolField(TEXT("cmd"), bCmd);

    const FName ActionFName(*ActionName);
    const FKey MappingKey(*Key);

    // 重复调用不应在配置里累积重复映射。
    if (bAxisMapping)
    {
        for (const FInputAxisKeyMapping& Existing : InputSettings->GetAxisMappings())
        {
            if (Existing.AxisName == ActionFName && Existing.Key == MappingKey)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Axis mapping already exists: %s -> %s"), *ActionName, *Key));
            }
        }
        FInputAxisKeyMapping AxisMapping;
        AxisMapping.AxisName = ActionFName;
        AxisMapping.Key = MappingKey;
        AxisMapping.Scale = 1.0f;
        InputSettings->AddAxisMapping(AxisMapping);
    }
    else
    {
        for (const FInputActionKeyMapping& Existing : InputSettings->GetActionMappings())
        {
            if (Existing.ActionName == ActionFName && Existing.Key == MappingKey)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Action mapping already exists: %s -> %s"), *ActionName, *Key));
            }
        }
        FInputActionKeyMapping ActionMapping;
        ActionMapping.ActionName = ActionFName;
        ActionMapping.Key = MappingKey;
        ActionMapping.bShift = bShift;
        ActionMapping.bCtrl = bCtrl;
        ActionMapping.bAlt = bAlt;
        ActionMapping.bCmd = bCmd;
        InputSettings->AddActionMapping(ActionMapping);
    }
    InputSettings->SaveConfig();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("action_name"), ActionName);
    ResultObj->SetStringField(TEXT("key"), Key);
    ResultObj->SetStringField(TEXT("input_type"), bAxisMapping ? TEXT("Axis") : TEXT("Action"));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params)
{
    // Create a new UDataTable asset at 'asset_path' using the row struct at 'struct_path'.
    // e.g. asset_path="/Game/Data/DT_Example", struct_path="/Game/Data/ST_Example"
    FString AssetPath, StructPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("struct_path"), StructPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'struct_path' parameter"));
    }

    UScriptStruct* RowStruct = Cast<UScriptStruct>(LoadObject<UObject>(nullptr, *StructPath));
    if (!RowStruct)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Row struct not found: %s"), *StructPath));
    }

    // Previous implementation used GetTransientPackage(), which returned success but
    // created an unsaved, unfindable table. Create the asset through AssetTools so it
    // appears under Content Browser and can be loaded by GameMode after an editor restart.
    const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    if (PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath));
    }

    FString PathError;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(AssetPath, AssetName, PathError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
    }

    UDataTable* NewTable = LoadObject<UDataTable>(nullptr, *FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName));
    if (!NewTable)
    {
        UDataTableFactory* Factory = NewObject<UDataTableFactory>();
        Factory->Struct = RowStruct;
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        NewTable = Cast<UDataTable>(AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UDataTable::StaticClass(), Factory));
    }

    if (!NewTable)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create persistent UDataTable asset"));
    }

    // 既有行内存必须与原行结构一致，重复创建不能替换结构。
    if (NewTable->RowStruct != RowStruct)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Existing DataTable has a different row struct"));
    }
    NewTable->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveLoadedAsset(NewTable, false))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("DataTable created but failed to save: %s"), *AssetPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("struct_path"), StructPath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleAddRow(const TSharedPtr<FJsonObject>& Params)
{
    // Add a row to an existing DataTable.
    // 'table_path' = DataTable asset path, 'row_name' = row key, 'row_data' = JSON object of struct fields.
    FString TablePath, RowName;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'table_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("row_name"), RowName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'row_name' parameter"));
    }
    const TSharedPtr<FJsonObject>* RowDataPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("row_data"), RowDataPtr) || !RowDataPtr)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'row_data' parameter"));
    }

    UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
    if (!Table)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
    }
    if (!Table->RowStruct)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("DataTable has no row struct"));
    }
    if (FName(*RowName).IsNone())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid row_name"));
    }

    // 由作用域负责初始化、销毁结构体成员和释放内存，失败路径也不会泄漏。
    FStructOnScope StructOnScope(Table->RowStruct);
    uint8* RowData = StructOnScope.GetStructMemory();
    if (!FJsonObjectConverter::JsonObjectToUStruct(RowDataPtr->ToSharedRef(), Table->RowStruct, RowData, 0, 0))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to convert row_data JSON to struct"));
    }
    Table->AddRow(FName(*RowName), *reinterpret_cast<FTableRowBase*>(RowData));
    Table->MarkPackageDirty();
    // UEditorAssetLibrary::SaveLoadedAsset proved unreliable for this asset in
    // UE4.27 (returned false with no detail). Save the package directly instead.
    UPackage* TablePackage = Table->GetOutermost();
    const FString TableFilename = FPackageName::LongPackageNameToFilename(TablePackage->GetName(), FPackageName::GetAssetPackageExtension());
    const bool bTableSaved = UPackage::SavePackage(TablePackage, Table, RF_Public | RF_Standalone,
        *TableFilename, GError, nullptr, false, true, SAVE_NoError);
    if (!bTableSaved)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Row added but failed to save DataTable: %s"), *TablePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleReadRow(const TSharedPtr<FJsonObject>& Params)
{
    // Read a row from a DataTable and return it as JSON.
    FString TablePath, RowName;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'table_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("row_name"), RowName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'row_name' parameter"));
    }

    UDataTable* Table = LoadObject<UDataTable>(nullptr, *TablePath);
    if (!Table)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
    }
    if (!Table->RowStruct)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("DataTable has no row struct"));
    }

    uint8* RowData = Table->FindRowUnchecked(FName(*RowName));
    if (!RowData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Row not found: %s"), *RowName));
    }

    TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
    // UStructToJsonObject takes a TSharedRef<FJsonObject> for the output.
    if (!FJsonObjectConverter::UStructToJsonObject(Table->RowStruct, RowData, RowJson.ToSharedRef(), 0, 0))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to serialize DataTable row"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    ResultObj->SetObjectField(TEXT("row_data"), RowJson);
    return ResultObj;
}
TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleSetAssetProperty(const TSharedPtr<FJsonObject>& Params)
{
    // 参数：asset_path（完整包路径，如 /Game/.../DA_Example）、property_name、property_value。
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
    }
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }
    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
    }

    const TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));
    FString ErrorMessage;
    if (!FUnrealMCPCommonUtils::SetObjectProperty(Asset, PropertyName, JsonValue, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set property: %s"), *ErrorMessage));
    }

    Asset->MarkPackageDirty();
    // 沿用 add_row 的直接保存方式：UEditorAssetLibrary 对此类资产不可靠。
    UPackage* AssetPackage = Asset->GetOutermost();
    const FString AssetFilename = FPackageName::LongPackageNameToFilename(AssetPackage->GetName(), FPackageName::GetAssetPackageExtension());
    const bool bSaved = UPackage::SavePackage(AssetPackage, Asset, RF_Public | RF_Standalone,
        *AssetFilename, GError, nullptr, false, true, SAVE_NoError);
    if (!bSaved)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Property set but failed to save asset: %s"), *AssetPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("property"), PropertyName);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleRefreshProjectReflection(const TSharedPtr<FJsonObject>& Params)
{
    // C++ 新增 BlueprintCallable 后，未重载模块时节点缓存仍是旧反射。
    // 这里做的不是重载模块（那需要 UBT 重编），而是让已加载的图按当前反射重建节点。
    FString BlueprintFilter;
    Params->TryGetStringField(TEXT("blueprint_name"), BlueprintFilter);

    TArray<UBlueprint*> Targets;
    if (!BlueprintFilter.IsEmpty())
    {
        UBlueprint* Only = FUnrealMCPCommonUtils::FindBlueprint(BlueprintFilter);
        if (!Only)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintFilter));
        }
        Targets.Add(Only);
    }
    else
    {
        const FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Assets;
        RegistryModule.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(), Assets, true);
        for (const FAssetData& Asset : Assets)
        {
            if (!Asset.ObjectPath.ToString().StartsWith(TEXT("/Game/"))) { continue; }
            if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
            {
                Targets.Add(Blueprint);
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> Refreshed;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (UBlueprint* Blueprint : Targets)
    {
        if (!Blueprint)
        {
            continue;
        }
        // 重建全图节点，让新增/改签名的 C++ 函数节点重新绑定反射。
        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        TSharedPtr<FJsonObject> Diagnostics;
        FString CompileError;
        if (!FUnrealMCPCommonUtils::CompileBlueprintWithDiagnostics(Blueprint, Diagnostics, CompileError))
        {
            auto Failure = MakeShared<FJsonObject>();
            Failure->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
            Failure->SetStringField(TEXT("error"), CompileError);
            Failure->SetObjectField(TEXT("diagnostics"), Diagnostics);
            Failed.Add(MakeShared<FJsonValueObject>(Failure));
            continue;
        }
        Refreshed.Add(MakeShared<FJsonValueString>(Blueprint->GetPathName()));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetBoolField(TEXT("success"), Failed.Num() == 0);
    if (Failed.Num()) { ResultData->SetStringField(TEXT("error"), TEXT("Some Blueprints failed to compile; see failed diagnostics")); }
    ResultData->SetArrayField(TEXT("refreshed"), Refreshed);
    ResultData->SetArrayField(TEXT("failed"), Failed);
    ResultData->SetNumberField(TEXT("refreshed_count"), Refreshed.Num());
    // 说明边界：本命令不重编 C++ 模块，函数不存在时仍需先构建模块。
    ResultData->SetStringField(TEXT("note"),
        TEXT("Refreshes graph nodes against currently loaded reflection. If a C++ function is still missing, rebuild the project modules first."));
    return ResultData;
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleCompileProject(const TSharedPtr<FJsonObject>& Params)
{
    // 通过控制台命令触发热编译，避免对可选插件产生编译期依赖。
    // 这只能覆盖「改了函数体」；新增 BlueprintCallable 是否进反射取决于 LiveCoding 的补丁范围，
    // 所以在它之后仍需调用 refresh_project_reflection 重建节点。
    if (!GEngine)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Engine unavailable"));
    }
    // 命令由插件注册，未启用 LiveCoding 时压根不存在；先查命令表再执行，
    // 否则 Exec 的失败无法区分「没有该命令」和「命令执行失败」。
    if (!IConsoleManager::Get().FindConsoleObject(TEXT("LiveCoding.Compile")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("LiveCoding.Compile is unavailable. Enable the Live Coding plugin and restart the editor, or rebuild the project modules with your build tool."));
    }

    // LiveCoding completion has no stable public callback; report request acceptance only.
    // 调用方随后用 refresh_project_reflection 重建节点，编译失败会体现在该步的 failed 列表里。
    if (!IConsoleManager::Get().ProcessUserConsoleInput(TEXT("LiveCoding.Compile"), *GLog, nullptr))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("LiveCoding compile request was not accepted"));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetBoolField(TEXT("compile_triggered"), true);
    ResultData->SetStringField(TEXT("note"),
        TEXT("LiveCoding compile is asynchronous; this only reports that the request was accepted. Call refresh_project_reflection afterwards to rebind graph nodes."));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPProjectCommands::HandleSaveAssets(const TSharedPtr<FJsonObject>& Params)
{
    // 配合各写命令的 defer_save=true：客户端决定何时落盘，失败资产逐项报告。
    const TArray<TSharedPtr<FJsonValue>>* AssetPaths = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPaths) || !AssetPaths || AssetPaths->Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_paths' parameter"));
    }

    TArray<TSharedPtr<FJsonValue>> Saved;
    TArray<TSharedPtr<FJsonValue>> Failed;
    for (const TSharedPtr<FJsonValue>& Entry : *AssetPaths)
    {
        FString AssetPath;
        if (!Entry.IsValid() || !Entry->TryGetString(AssetPath) || !AssetPath.StartsWith(TEXT("/Game/")))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("asset_paths must contain /Game asset paths"));
        }
    }
    for (const TSharedPtr<FJsonValue>& Entry : *AssetPaths)
    {
        const FString AssetPath = Entry->AsString();
        UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
        if (!Asset)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("asset_path"), AssetPath);
            Item->SetStringField(TEXT("error"), TEXT("Asset not found"));
            Failed.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }
        if (Asset->IsA<UWorld>())
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("asset_path"), AssetPath);
            Item->SetStringField(TEXT("error"), TEXT("Use save_level to save maps"));
            Failed.Add(MakeShared<FJsonValueObject>(Item));
            continue;
        }
        FString SaveError;
        if (FUnrealMCPCommonUtils::SaveLoadedAssetReliable(Asset, SaveError))
        {
            Saved.Add(MakeShared<FJsonValueString>(AssetPath));
        }
        else
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("asset_path"), AssetPath);
            Item->SetStringField(TEXT("error"), SaveError);
            Failed.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetArrayField(TEXT("saved"), Saved);
    ResultData->SetArrayField(TEXT("failed"), Failed);
    ResultData->SetBoolField(TEXT("success"), Failed.Num() == 0);
    if (Failed.Num()) { ResultData->SetStringField(TEXT("error"), TEXT("Some assets could not be saved; earlier saves may have succeeded")); }
    return ResultData;
}
