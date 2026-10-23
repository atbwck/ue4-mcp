#include "Commands/UnrealMCPMaterialCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "MCPMaterialGraphLayout.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialEditingLibrary.h"
#include "StaticParameterSet.h"

#include "EditorAssetLibrary.h"
#include "AssetRegistryModule.h"
#include "Components/MeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Texture.h"
#include "FileHelpers.h"
#include "ScopedTransaction.h"

namespace
{
    // 寻址：调用方常只给资产名，命令必须能自己补成引擎认的路径。
    FString NormalizeAssetPath(const FString& Input)
    {
        FString Path = Input;
        Path.TrimStartAndEndInline();
        if (Path.IsEmpty())
        {
            return Path;
        }
        if (Path.Contains(TEXT("/")) && !Path.Contains(TEXT(".")))
        {
            const FString AssetName = FPackageName::GetShortName(Path);
            Path = Path + TEXT(".") + AssetName;
        }
        return Path;
    }

    UObject* LoadAssetByAnyName(const FString& Input)
    {
        const FString Direct = NormalizeAssetPath(Input);
        if (Direct.Contains(TEXT(".")))
        {
            if (UObject* Loaded = LoadObject<UObject>(nullptr, *Direct))
            {
                return Loaded;
            }
        }
        if (UObject* Loaded = UEditorAssetLibrary::LoadAsset(Direct))
        {
            return Loaded;
        }

        // 裸名回退：先查 /Game/Materials，再按资产注册表全名查找。
        if (!Input.Contains(TEXT("/")))
        {
            const FString Conventional = FString::Printf(TEXT("/Game/Materials/%s.%s"), *Input, *Input);
            if (UObject* Loaded = LoadObject<UObject>(nullptr, *Conventional))
            {
                return Loaded;
            }
            const FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FAssetData> Matches;
            RegistryModule.Get().GetAssetsByPath(FName(TEXT("/Game")), Matches, true);
            for (const FAssetData& Data : Matches)
            {
                if (Data.AssetName.ToString() == Input)
                {
                    if (UObject* Loaded = Data.GetAsset())
                    {
                        return Loaded;
                    }
                }
            }
        }
        return nullptr;
    }

    UMaterial* ResolveMaterial(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString MaterialPath;
        if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath) &&
            !Params->TryGetStringField(TEXT("material_name"), MaterialPath))
        {
            OutError = TEXT("Missing 'material_path' parameter");
            return nullptr;
        }
        UMaterial* Material = Cast<UMaterial>(LoadAssetByAnyName(MaterialPath));
        if (!Material)
        {
            OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
            return nullptr;
        }
        return Material;
    }

    UMaterialInterface* ResolveMaterialInterface(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutError)
    {
        FString Path;
        if (!Params->TryGetStringField(FieldName, Path))
        {
            OutError = FString::Printf(TEXT("Missing '%s' parameter"), FieldName);
            return nullptr;
        }
        UMaterialInterface* MaterialInterface = Cast<UMaterialInterface>(LoadAssetByAnyName(Path));
        if (!MaterialInterface)
        {
            OutError = FString::Printf(TEXT("Material interface not found: %s"), *Path);
            return nullptr;
        }
        return MaterialInterface;
    }

    UMaterialInstanceConstant* ResolveMaterialInstance(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("instance_path"), Path) &&
            !Params->TryGetStringField(TEXT("material_instance_path"), Path))
        {
            OutError = TEXT("Missing 'instance_path' parameter");
            return nullptr;
        }
        UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(LoadAssetByAnyName(Path));
        if (!Instance)
        {
            OutError = FString::Printf(TEXT("Material instance constant not found: %s"), *Path);
            return nullptr;
        }
        return Instance;
    }

    FString NodeIdOf(UMaterialExpression* Expression)
    {
        if (!Expression)
        {
            return FString();
        }
        FGuid& Guid = Expression->GetMaterialExpressionId();
        return Guid.IsValid() ? Guid.ToString() : Expression->GetName();
    }

    UMaterialExpression* FindExpression(UMaterial* Material, const FString& NodeId, bool& bAmbiguous)
    {
        bAmbiguous = false;
        UMaterialExpression* Match = nullptr;
        for (UMaterialExpression* Expression : Material->Expressions)
        {
            if (!Expression)
            {
                continue;
            }
            if (NodeIdOf(Expression) == NodeId || Expression->GetName() == NodeId)
            {
                if (Match && Match != Expression)
                {
                    bAmbiguous = true;
                    return nullptr;
                }
                Match = Expression;
            }
        }
        for (UMaterialExpressionComment* Comment : Material->EditorComments)
        {
            if (Comment && (NodeIdOf(Comment) == NodeId || Comment->GetName() == NodeId))
            {
                if (Match && Match != Comment)
                {
                    bAmbiguous = true;
                    return nullptr;
                }
                Match = Comment;
            }
        }
        return Match;
    }

    UMaterialExpression* RequireExpression(UMaterial* Material, const TSharedPtr<FJsonObject>& Params,
                                          const TCHAR* FieldName, FString& OutError)
    {
        FString NodeId;
        if (!Params->TryGetStringField(FieldName, NodeId))
        {
            OutError = FString::Printf(TEXT("Missing '%s' parameter"), FieldName);
            return nullptr;
        }
        bool bAmbiguous = false;
        UMaterialExpression* Expression = FindExpression(Material, NodeId, bAmbiguous);
        if (bAmbiguous)
        {
            OutError = FString::Printf(TEXT("'%s' matches more than one node"), FieldName);
            return nullptr;
        }
        if (!Expression)
        {
            OutError = FString::Printf(TEXT("Node not found: %s"), *NodeId);
            return nullptr;
        }
        return Expression;
    }

    // 引擎的输入按顺序排列，未命名的第一个输入是空名字。空输入名表示第 0 个输入。
    FExpressionInput* FindInput(UMaterialExpression* Expression, int32 InputIndex, FName InputName)
    {
        TArray<FExpressionInput*> Inputs = Expression->GetInputs();
        if (!InputName.IsNone())
        {
            for (int32 Index = 0; Index < Inputs.Num(); ++Index)
            {
                const FName ActualName = Expression->IsA<UMaterialExpressionMaterialFunctionCall>()
                    ? CastChecked<UMaterialExpressionMaterialFunctionCall>(Expression)->GetInputNameWithType(Index, false)
                    : Expression->GetInputName(Index);
                if (ActualName == InputName)
                {
                    return Inputs[Index];
                }
            }
            return nullptr;
        }
        return InputIndex >= 0 && InputIndex < Inputs.Num() ? Inputs[InputIndex] : nullptr;
    }

    int32 FindOutputIndex(UMaterialExpression* Expression, const FString& OutputName, int32 FallbackIndex)
    {
        if (OutputName.IsEmpty())
        {
            const int32 Index = FallbackIndex >= 0 ? FallbackIndex : 0;
            return Expression->GetOutputs().IsValidIndex(Index) ? Index : INDEX_NONE;
        }
        const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
        for (int32 Index = 0; Index < Outputs.Num(); ++Index)
        {
            const FExpressionOutput& Output = Outputs[Index];
            if (Output.OutputName.ToString() == OutputName) return Index;
            if (!Output.OutputName.IsNone()) continue;
            if (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA && OutputName == TEXT("R")) return Index;
            if (!Output.MaskR && Output.MaskG && !Output.MaskB && !Output.MaskA && OutputName == TEXT("G")) return Index;
            if (!Output.MaskR && !Output.MaskG && Output.MaskB && !Output.MaskA && OutputName == TEXT("B")) return Index;
            if (!Output.MaskR && !Output.MaskG && !Output.MaskB && Output.MaskA && OutputName == TEXT("A")) return Index;
        }
        return INDEX_NONE;
    }

    bool ReadInputLocator(const TSharedPtr<FJsonObject>& Params, int32& OutIndex, FName& OutName, FString& OutError)
    {
        OutIndex = 0;
        OutName = NAME_None;
        FString InputName;
        const bool bHasName = Params->TryGetStringField(TEXT("to_input"), InputName);
        const bool bHasIndex = Params->HasField(TEXT("to_input_index"));
        if (bHasName && bHasIndex)
        {
            OutError = TEXT("Provide only one of 'to_input' or 'to_input_index'");
            return false;
        }
        if (bHasName)
        {
            if (InputName.Len() >= NAME_SIZE)
            {
                OutError = TEXT("to_input is too long");
                return false;
            }
            OutName = FName(*InputName);
            return true;
        }
        double IndexValue = 0.0;
        if (bHasIndex && Params->TryGetNumberField(TEXT("to_input_index"), IndexValue))
        {
            if (!FMath::IsFinite(IndexValue) || FMath::FloorToDouble(IndexValue) != IndexValue ||
                IndexValue < 0 || IndexValue > MAX_int32)
            {
                OutError = TEXT("'to_input_index' must be a non-negative int32");
                return false;
            }
            OutIndex = static_cast<int32>(IndexValue);
            return true;
        }
        if (bHasIndex)
        {
            OutError = TEXT("'to_input_index' must be a number");
            return false;
        }
        OutError = TEXT("Provide 'to_input' or 'to_input_index'");
        return false;
    }

    bool ReadOutputLocator(const TSharedPtr<FJsonObject>& Params, FString& OutName, int32& OutIndex, FString& OutError)
    {
        OutName.Reset();
        OutIndex = -1;
        const bool bHasName = Params->TryGetStringField(TEXT("from_output"), OutName);
        const bool bHasIndex = Params->HasField(TEXT("from_output_index"));
        if (bHasName && bHasIndex)
        {
            OutError = TEXT("Provide only one of 'from_output' or 'from_output_index'");
            return false;
        }
        if (bHasName) return true;
        double IndexValue = 0.0;
        if (bHasIndex && Params->TryGetNumberField(TEXT("from_output_index"), IndexValue))
        {
            if (!FMath::IsFinite(IndexValue) || FMath::FloorToDouble(IndexValue) != IndexValue ||
                IndexValue < 0 || IndexValue > MAX_int32)
            {
                OutError = TEXT("'from_output_index' must be a non-negative int32");
                return false;
            }
            OutIndex = static_cast<int32>(IndexValue);
            return true;
        }
        if (bHasIndex)
        {
            OutError = TEXT("'from_output_index' must be a number");
            return false;
        }
        // 省略输出时取第一个输出，与引擎的「第一个未命名输出」一致
        OutIndex = 0;
        return true;
    }

    // 材质属性名接受 MP_BaseColor、BaseColor 或显示名 Diffuse。
    FString MaterialPropertyName(EMaterialProperty Property)
    {
        const UEnum* Enum = StaticEnum<EMaterialProperty>();
        if (!Enum)
        {
            return FString::FromInt(static_cast<int32>(Property));
        }
        FString Name = Enum->GetNameStringByValue(static_cast<int64>(Property));
        Name.RemoveFromStart(TEXT("MP_"));
        return Name;
    }

    bool ResolveMaterialProperty(const FString& Input, EMaterialProperty& OutProperty, FString& OutError)
    {
        const UEnum* Enum = StaticEnum<EMaterialProperty>();
        if (!Enum)
        {
            OutError = TEXT("EMaterialProperty is unavailable");
            return false;
        }
        FString Wanted = Input;
        Wanted.TrimStartAndEndInline();
        Wanted.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
        for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
        {
            FString Candidate = Enum->GetNameStringByIndex(Index);
            Candidate.RemoveFromStart(TEXT("MP_"));
            if (Candidate.Equals(Wanted, ESearchCase::IgnoreCase) ||
                Enum->GetNameStringByIndex(Index).Equals(Input, ESearchCase::IgnoreCase) ||
                Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Input, ESearchCase::IgnoreCase))
            {
                const int64 Value = Enum->GetValueByIndex(Index);
                if (Value < 0 || Value >= MP_MAX)
                {
                    break;
                }
                OutProperty = static_cast<EMaterialProperty>(Value);
                return true;
            }
        }
        OutError = FString::Printf(
            TEXT("Unknown material property: %s. Use a name like BaseColor, Metallic, Roughness, Normal, EmissiveColor, Opacity or OpacityMask"),
            *Input);
        return false;
    }

    UClass* FindExpressionClass(const FString& Input)
    {
        FString Wanted = Input;
        Wanted.TrimStartAndEndInline();
        if (Wanted.StartsWith(TEXT("/")))
        {
            if (UClass* Direct = LoadClass<UMaterialExpression>(nullptr, *Wanted))
            {
                return Direct;
            }
        }
        // 接受全名、去掉前缀的短名和去掉 U 的类名
        const FString WithoutU = Wanted.StartsWith(TEXT("U")) ? Wanted.Mid(1) : Wanted;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!Candidate || !Candidate->IsChildOf(UMaterialExpression::StaticClass()) || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
            {
                continue;
            }
            const FString ClassName = Candidate->GetName();
            if (ClassName.Equals(Wanted, ESearchCase::IgnoreCase) ||
                ClassName.Equals(WithoutU, ESearchCase::IgnoreCase) ||
                (ClassName.StartsWith(TEXT("MaterialExpression")) &&
                 ClassName.Mid(FCString::Strlen(TEXT("MaterialExpression"))).Equals(Wanted, ESearchCase::IgnoreCase)))
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    // 位置由排版决定，手工坐标只在调用方显式给出时使用。
    void PlaceExpressionWithoutOverlap(UMaterial* Material, UMaterialExpression* Expression)
    {
        int32 MaxBottom = 0;
        for (UMaterialExpression* Other : Material->Expressions)
        {
            if (Other && Other != Expression)
            {
                MaxBottom = FMath::Max(MaxBottom, Other->MaterialExpressionEditorY + Other->GetHeight());
            }
        }
        Expression->MaterialExpressionEditorX = 0;
        Expression->MaterialExpressionEditorY = ((MaxBottom + 63) / 64) * 64;
    }

    bool ApplyJsonProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, TArray<FString>& OutApplied, FString& OutError)
    {
        if (!Properties.IsValid())
        {
            return true;
        }
        TStrongObjectPtr<UObject> Preview(DuplicateObject<UObject>(Target, GetTransientPackage()));
        for (const auto& Pair : Properties->Values)
        {
            if (Pair.Key.IsEmpty() || Pair.Key.Len() >= NAME_SIZE)
            {
                OutError = TEXT("Property name is empty or too long");
                return false;
            }
            FProperty* Property = Target->GetClass()->FindPropertyByName(*Pair.Key);
            if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) ||
                Property->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance))
            {
                OutError = FString::Printf(TEXT("Property is not editable: %s"), *Pair.Key);
                return false;
            }
            FString Message;
            if (!FUnrealMCPCommonUtils::SetObjectProperty(Preview.Get(), Pair.Key, Pair.Value, Message))
            {
                OutError = FString::Printf(TEXT("%s: %s"), *Pair.Key, *Message);
                return false;
            }
        }
        for (const auto& Pair : Properties->Values)
        {
            FProperty* Property = Target->GetClass()->FindPropertyByName(*Pair.Key);
            Property->CopyCompleteValue_InContainer(Target, Preview.Get());
            OutApplied.Add(Pair.Key);
        }
        return true;
    }

    TSharedPtr<FJsonObject> ExpressionPayload(UMaterialExpression* Expression)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("node_id"), NodeIdOf(Expression));
        Item->SetStringField(TEXT("name"), Expression->GetName());
        Item->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
        TArray<FString> Captions;
        Expression->GetCaption(Captions);
        Item->SetStringField(TEXT("caption"), FString::Join(Captions, TEXT(" ")));
        Item->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
        Item->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
        if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
        {
            Item->SetStringField(TEXT("parameter_name"), Parameter->ParameterName.ToString());
            Item->SetStringField(TEXT("parameter_group"), Parameter->Group.ToString());
        }
        return Item;
    }

    void RecompileAndRefresh(UMaterial* Material)
    {
        UMaterialEditingLibrary::RecompileMaterial(Material);
        Material->MarkPackageDirty();
    }

    bool SaveIfNeeded(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        return FUnrealMCPCommonUtils::SaveUnlessDeferred(Asset, Params, OutError);
    }

    TSharedPtr<FJsonObject> FinishGraphEdit(UMaterial* Material, const TSharedPtr<FJsonObject>& Params,
                                          const TSharedPtr<FJsonObject>& Result)
    {
        RecompileAndRefresh(Material);
        FString Error;
        if (!SaveIfNeeded(Material, Params, Error))
        {
            auto Failure = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material changed but save failed: ") + Error);
            Failure->SetObjectField(TEXT("data"), Result);
            return Failure;
        }
        Result->SetStringField(TEXT("material_path"), Material->GetPathName());
        Result->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Result);
    }

    bool ReadLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FLinearColor& OutColor, FString& OutError)
    {
        if (!Params->HasField(FieldName)) return false;
        const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
        if (!Params->TryGetArrayField(FieldName, Channels))
        {
            OutError = FString::Printf(TEXT("'%s' must be an array of 3 or 4 finite numbers"), FieldName);
            return false;
        }
        if (Channels->Num() < 3 || Channels->Num() > 4)
        {
            OutError = FString::Printf(TEXT("'%s' needs 3 or 4 numbers"), FieldName);
            return true;
        }
        double Values[4] = {0.0, 0.0, 0.0, 1.0};
        for (int32 Index = 0; Index < Channels->Num(); ++Index)
        {
            if (!(*Channels)[Index].IsValid() || !(*Channels)[Index]->TryGetNumber(Values[Index]) ||
                !FMath::IsFinite(Values[Index]) || FMath::Abs(Values[Index]) > MAX_flt)
            {
                OutError = FString::Printf(TEXT("'%s' needs finite numbers"), FieldName);
                return true;
            }
        }
        OutColor = FLinearColor((float)Values[0], (float)Values[1], (float)Values[2], (float)Values[3]);
        return true;
    }

    // 设置名和引擎枚举名差一个前缀，两个都接受。
    bool ResolveEnumValueByName(const UEnum* Enum, const FString& Input, const TCHAR* Prefix, int64& OutValue)
    {
        if (!Enum)
        {
            return false;
        }
        FString Wanted = Input;
        Wanted.TrimStartAndEndInline();
        TArray<FString> Candidates;
        Candidates.Add(Wanted);
        if (!Wanted.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            Candidates.Add(FString(Prefix) + Wanted);
        }
        for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
        {
            const FString Name = Enum->GetNameStringByIndex(Index);
            if (Name.EndsWith(TEXT("_MAX")) || Name.EndsWith(TEXT("_NUM")))
            {
                continue;
            }
            for (const FString& Candidate : Candidates)
            {
                if (Name.Equals(Candidate, ESearchCase::IgnoreCase))
                {
                    OutValue = Enum->GetValueByIndex(Index);
                    return true;
                }
            }
        }
        return false;
    }

}

FUnrealMCPMaterialCommands::FUnrealMCPMaterialCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("create_material"))
    {
        return HandleCreateMaterial(Params);
    }
    if (CommandType == TEXT("add_vector_parameter"))
    {
        return HandleAddVectorParameter(Params);
    }
    if (CommandType == TEXT("set_material_on_component"))
    {
        return HandleSetMaterialOnComponent(Params);
    }
    if (CommandType == TEXT("create_dynamic_material_instance"))
    {
        return HandleCreateDynamicMaterialInstance(Params);
    }
    if (CommandType == TEXT("set_material_parameter"))
    {
        return HandleSetMaterialParameter(Params);
    }
    if (CommandType == TEXT("get_component_materials"))
    {
        return HandleGetComponentMaterials(Params);
    }
    if (CommandType == TEXT("get_material_graph"))
    {
        return HandleGetMaterialGraph(Params);
    }
    if (CommandType == TEXT("list_material_parameters"))
    {
        return HandleListMaterialParameters(Params);
    }
    if (CommandType == TEXT("add_material_expression"))
    {
        return HandleAddMaterialExpression(Params);
    }
    if (CommandType == TEXT("set_material_expression_property"))
    {
        return HandleSetMaterialExpressionProperty(Params);
    }
    if (CommandType == TEXT("delete_material_expression"))
    {
        return HandleDeleteMaterialExpression(Params);
    }
    if (CommandType == TEXT("connect_material_expressions"))
    {
        return HandleConnectMaterialExpressions(Params);
    }
    if (CommandType == TEXT("disconnect_material_expressions"))
    {
        return HandleDisconnectMaterialExpressions(Params);
    }
    if (CommandType == TEXT("connect_material_property"))
    {
        return HandleConnectMaterialProperty(Params);
    }
    if (CommandType == TEXT("disconnect_material_property"))
    {
        return HandleDisconnectMaterialProperty(Params);
    }
    if (CommandType == TEXT("set_material_settings"))
    {
        return HandleSetMaterialSettings(Params);
    }
    if (CommandType == TEXT("add_material_comment"))
    {
        return HandleAddMaterialComment(Params);
    }
    if (CommandType == TEXT("format_material_graph"))
    {
        return HandleFormatMaterialGraph(Params);
    }
    if (CommandType == TEXT("create_material_instance"))
    {
        return HandleCreateMaterialInstance(Params);
    }
    if (CommandType == TEXT("set_material_instance_parameter"))
    {
        return HandleSetMaterialInstanceParameter(Params);
    }
    if (CommandType == TEXT("get_material_instance_parameters"))
    {
        return HandleGetMaterialInstanceParameters(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown material command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCreateMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialName;
    if (!Params->TryGetStringField(TEXT("material_name"), MaterialName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_name' parameter"));
    }

    FString SavePath = TEXT("/Game/Materials");
    Params->TryGetStringField(TEXT("save_path"), SavePath);
    SavePath.TrimStartAndEndInline();
    if (SavePath.EndsWith(TEXT("/")))
    {
        SavePath = SavePath.LeftChop(1);
    }

    const FString PackagePath = SavePath + TEXT("/") + MaterialName;
    FString PathError;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(PackagePath, MaterialName, PathError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
    }
    if (UEditorAssetLibrary::DoesAssetExist(PackagePath + TEXT(".") + MaterialName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material already exists: %s"), *PackagePath));
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package for material"));
    }

    UMaterial* Material = NewObject<UMaterial>(Package, *MaterialName, RF_Public | RF_Standalone | RF_Transactional);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create material object"));
    }

    FAssetRegistryModule::AssetCreated(Material);
    Material->MarkPackageDirty();

    FString SaveError;
    if (!SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Material created but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_name"), MaterialName);
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("package_path"), PackagePath);
    ResultData->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddVectorParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'param_name' parameter"));
    }

    if (ParamName.IsEmpty() || ParamName.Len() >= NAME_SIZE)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("param_name is empty or too long"));
    FLinearColor DefaultColor(0.5f, 0.5f, 0.5f, 1.0f);
    FString ColorError;
    const TSharedPtr<FJsonObject>* ColorObject = nullptr;
    if (Params->TryGetObjectField(TEXT("default_color"), ColorObject) && ColorObject && ColorObject->IsValid())
    {
        const TCHAR* Channels[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
        float* Values[] = {&DefaultColor.R, &DefaultColor.G, &DefaultColor.B, &DefaultColor.A};
        for (int32 Index = 0; Index < 4; ++Index)
        {
            if (!(*ColorObject)->HasField(Channels[Index])) continue;
            double Channel = 0.0;
            if (!(*ColorObject)->TryGetNumberField(Channels[Index], Channel) || !FMath::IsFinite(Channel) || FMath::Abs(Channel) > MAX_flt)
                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("default_color channels must be finite floats"));
            *Values[Index] = static_cast<float>(Channel);
        }
    }
    else
    {
        ReadLinearColor(Params, TEXT("default_color"), DefaultColor, ColorError);
        if (!ColorError.IsEmpty()) return FUnrealMCPCommonUtils::CreateErrorResponse(ColorError);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddVectorParameter", "UnrealMCP Add Vector Parameter"));
    Material->Modify();

    UMaterialExpressionVectorParameter* VectorParam = Cast<UMaterialExpressionVectorParameter>(
        UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), 0, 0));
    if (!VectorParam)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create vector parameter expression"));
    }
    VectorParam->ParameterName = FName(*ParamName);
    VectorParam->DefaultValue = DefaultColor;
    VectorParam->Desc = FString::Printf(TEXT("Vector parameter '%s' added via MCP"), *ParamName);
    PlaceExpressionWithoutOverlap(Material, VectorParam);

    if (!UMaterialEditingLibrary::ConnectMaterialProperty(VectorParam, FString(), MP_BaseColor))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Vector parameter created but connecting it to BaseColor failed"));
    }

    RecompileAndRefresh(Material);
    MCPMaterialGraphLayout::Format(Material, MakeShared<FJsonObject>());

    FString SaveError;
    if (!SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Vector parameter added but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = ExpressionPayload(VectorParam);
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("param_name"), ParamName);
    ResultData->SetStringField(TEXT("connected_to"), TEXT("BaseColor"));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialOnComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }
    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }
    FString MaterialPath;
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }
    int32 MaterialIndex = 0;
    Params->TryGetNumberField(TEXT("material_index"), MaterialIndex);

    AActor* TargetActor = FindWorldActor(ActorName, Params);
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }
    UMeshComponent* TargetComponent = FindMeshComponent(TargetActor, ComponentName);
    if (!TargetComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
    }
    if (MaterialIndex < 0 || MaterialIndex >= TargetComponent->GetNumMaterials())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material index is out of range"));
    }

    UMaterialInterface* MaterialAsset = Cast<UMaterialInterface>(LoadAssetByAnyName(MaterialPath));
    if (!MaterialAsset)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Failed to load material: %s. Pass a full asset path such as /Game/Materials/M_Name.M_Name"), *MaterialPath));
    }

    if (!FUnrealMCPCommonUtils::ShouldDeferSave(Params) &&
        !TargetActor->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Save the level to a /Game path first, or use defer_save=true and save_level afterwards"));
    }
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AssignMaterial", "UnrealMCP Assign Material"));
    TargetActor->Modify();
    TargetComponent->Modify();
    TargetComponent->SetMaterial(MaterialIndex, MaterialAsset);
    TargetActor->MarkPackageDirty();
    if (!FUnrealMCPCommonUtils::ShouldDeferSave(Params) &&
        !FEditorFileUtils::SaveLevel(TargetActor->GetLevel()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material assigned but level save failed"));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("actor_name"), ActorName);
    ResultData->SetStringField(TEXT("component_name"), TargetComponent->GetName());
    ResultData->SetNumberField(TEXT("material_index"), MaterialIndex);
    ResultData->SetStringField(TEXT("material_path"), MaterialAsset->GetPathName());
    ResultData->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

AActor* FUnrealMCPMaterialCommands::FindWorldActor(const FString& ActorName, const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return nullptr;
    }
    bool bPreferPIE = false;
    if (Params)
    {
        Params->TryGetBoolField(TEXT("pie"), bPreferPIE);
    }
    // 动态材质实例只存在于运行时世界，查错世界会让参数写入静默失效。
    UWorld* World = bPreferPIE ? GEditor->PlayWorld : GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return nullptr;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It && It->GetName() == ActorName)
        {
            return *It;
        }
    }
    return nullptr;
}

UMeshComponent* FUnrealMCPMaterialCommands::FindMeshComponent(AActor* Actor, const FString& ComponentName)
{
    if (!Actor)
    {
        return nullptr;
    }
    TArray<UMeshComponent*> MeshComps;
    Actor->GetComponents<UMeshComponent>(MeshComps);
    for (UMeshComponent* MeshComp : MeshComps)
    {
        if (MeshComp && MeshComp->GetName() == ComponentName)
        {
            return MeshComp;
        }
    }
    return nullptr;
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCreateDynamicMaterialInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }
    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }
    int32 MaterialIndex = 0;
    Params->TryGetNumberField(TEXT("material_index"), MaterialIndex);

    AActor* TargetActor = FindWorldActor(ActorName, Params);
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Actor not found: %s. For runtime worlds pass pie=true."), *ActorName));
    }
    UMeshComponent* TargetComponent = FindMeshComponent(TargetActor, ComponentName);
    if (!TargetComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
    }
    if (MaterialIndex < 0 || MaterialIndex >= TargetComponent->GetNumMaterials())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material index is out of range"));
    }

    bool bPIE = false;
    Params->TryGetBoolField(TEXT("pie"), bPIE);
    if (!bPIE || !GEditor || !GEditor->PlayWorld)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Creating a DMI requires an active PIE session and pie=true"));
    }
    UMaterialInterface* CurrentMaterial = TargetComponent->GetMaterial(MaterialIndex);
    if (!CurrentMaterial)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Material slot %d is empty; assign a material before creating a dynamic instance"), MaterialIndex));
    }
    // 已有 DMI 时直接复用，避免每次调用都叠加一层实例。
    if (UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(CurrentMaterial))
    {
        TSharedPtr<FJsonObject> Reused = MakeShared<FJsonObject>();
        Reused->SetStringField(TEXT("actor_name"), ActorName);
        Reused->SetStringField(TEXT("component_name"), TargetComponent->GetName());
        Reused->SetNumberField(TEXT("material_index"), MaterialIndex);
        Reused->SetStringField(TEXT("dynamic_instance"), Existing->GetPathName());
        Reused->SetStringField(TEXT("parent_material"), Existing->Parent ? Existing->Parent->GetPathName() : FString());
        Reused->SetBoolField(TEXT("created"), false);
        return FUnrealMCPCommonUtils::CreateSuccessResponse(Reused);
    }

    UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(CurrentMaterial, TargetComponent);
    if (!Dynamic)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create dynamic material instance"));
    }
    TargetComponent->SetMaterial(MaterialIndex, Dynamic);

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("actor_name"), ActorName);
    ResultData->SetStringField(TEXT("component_name"), TargetComponent->GetName());
    ResultData->SetNumberField(TEXT("material_index"), MaterialIndex);
    ResultData->SetStringField(TEXT("dynamic_instance"), Dynamic->GetPathName());
    ResultData->SetStringField(TEXT("parent_material"), Dynamic->Parent ? Dynamic->Parent->GetPathName() : FString());
    ResultData->SetBoolField(TEXT("created"), true);
    ResultData->SetStringField(TEXT("note"), TEXT("Dynamic material instances live on the runtime object and are not persisted."));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }
    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }
    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'param_name' parameter"));
    }
    int32 MaterialIndex = 0;
    Params->TryGetNumberField(TEXT("material_index"), MaterialIndex);

    AActor* TargetActor = FindWorldActor(ActorName, Params);
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Actor not found: %s. For runtime worlds pass pie=true."), *ActorName));
    }
    UMeshComponent* TargetComponent = FindMeshComponent(TargetActor, ComponentName);
    if (!TargetComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
    }
    if (MaterialIndex < 0 || MaterialIndex >= TargetComponent->GetNumMaterials())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material index is out of range"));
    }
    // 参数只能写动态实例；写到资产上会改到所有使用者，这是必须拒绝的误用。
    UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(TargetComponent->GetMaterial(MaterialIndex));
    if (!Dynamic)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Slot %d is not a dynamic material instance; call create_dynamic_material_instance first"), MaterialIndex));
    }

    const bool bScalar = Params->HasField(TEXT("scalar"));
    if (bScalar == Params->HasField(TEXT("vector")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide exactly one of scalar or vector"));
    }
    if (ParamName.IsEmpty() || ParamName.Len() >= NAME_SIZE)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("param_name is empty or too long"));
    const FMaterialParameterInfo ParameterInfo{FName(*ParamName)};
    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("param_name"), ParamName);
    if (bScalar)
    {
        double ScalarValue = 0.0;
        if (!Params->TryGetNumberField(TEXT("scalar"), ScalarValue) ||
            !FMath::IsFinite(ScalarValue) || FMath::Abs(ScalarValue) > MAX_flt)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'scalar' must be a number"));
        }
        float Previous = 0;
        if (!Dynamic->GetScalarParameterValue(ParameterInfo, Previous))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Scalar parameter not found on material"));
        }
        Dynamic->SetScalarParameterValue(FName(*ParamName), (float)ScalarValue);
        ResultData->SetStringField(TEXT("kind"), TEXT("scalar"));
        ResultData->SetNumberField(TEXT("value"), ScalarValue);
    }
    else
    {
        const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
        if (!Params->TryGetArrayField(TEXT("vector"), Channels) || Channels->Num() != 3)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("vector must contain three finite numbers"));
        }
        for (const auto& Channel : *Channels)
        {
            double Value;
            if (!Channel.IsValid() || !Channel->TryGetNumber(Value) ||
                !FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("vector must contain three finite numbers"));
            }
        }
        FLinearColor Previous;
        if (!Dynamic->GetVectorParameterValue(ParameterInfo, Previous))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Vector parameter not found on material"));
        }
        const FVector VectorValue = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("vector"));
        Dynamic->SetVectorParameterValue(FName(*ParamName), FLinearColor(VectorValue.X, VectorValue.Y, VectorValue.Z, 1.0f));
        ResultData->SetStringField(TEXT("kind"), TEXT("vector"));
        ResultData->SetStringField(TEXT("value"), VectorValue.ToString());
    }

    ResultData->SetStringField(TEXT("actor_name"), ActorName);
    ResultData->SetStringField(TEXT("component_name"), TargetComponent->GetName());
    ResultData->SetNumberField(TEXT("material_index"), MaterialIndex);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetComponentMaterials(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }
    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    AActor* TargetActor = FindWorldActor(ActorName, Params);
    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Actor not found: %s. For runtime worlds pass pie=true."), *ActorName));
    }
    UMeshComponent* TargetComponent = FindMeshComponent(TargetActor, ComponentName);
    if (!TargetComponent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Mesh component '%s' not found on actor '%s'"), *ComponentName, *ActorName));
    }

    TArray<TSharedPtr<FJsonValue>> Slots;
    const int32 NumMaterials = TargetComponent->GetNumMaterials();
    for (int32 Index = 0; Index < NumMaterials; ++Index)
    {
        UMaterialInterface* SlotMaterial = TargetComponent->GetMaterial(Index);
        TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
        Slot->SetNumberField(TEXT("material_index"), Index);
        Slot->SetStringField(TEXT("material_path"), SlotMaterial ? SlotMaterial->GetPathName() : FString());
        Slot->SetBoolField(TEXT("is_dynamic_instance"), SlotMaterial && SlotMaterial->IsA<UMaterialInstanceDynamic>());

        if (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(SlotMaterial))
        {
            Slot->SetStringField(TEXT("parent_material"), Dynamic->Parent ? Dynamic->Parent->GetPathName() : FString());
            // 把当前参数值回读出来，渲染验证才有据可依。
            TArray<FMaterialParameterInfo> ScalarInfos;
            TArray<FGuid> ScalarIds;
            Dynamic->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);
            TArray<TSharedPtr<FJsonValue>> Scalars;
            for (const FMaterialParameterInfo& Info : ScalarInfos)
            {
                float Value = 0.0f;
                if (Dynamic->GetScalarParameterValue(Info, Value))
                {
                    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
                    Item->SetStringField(TEXT("name"), Info.Name.ToString());
                    Item->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association));
                    Item->SetNumberField(TEXT("index"), Info.Index);
                    Item->SetNumberField(TEXT("value"), Value);
                    Scalars.Add(MakeShared<FJsonValueObject>(Item));
                }
            }
            TArray<FMaterialParameterInfo> VectorInfos;
            TArray<FGuid> VectorIds;
            Dynamic->GetAllVectorParameterInfo(VectorInfos, VectorIds);
            TArray<TSharedPtr<FJsonValue>> Vectors;
            for (const FMaterialParameterInfo& Info : VectorInfos)
            {
                FLinearColor Value = FLinearColor::Black;
                if (Dynamic->GetVectorParameterValue(Info, Value))
                {
                    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
                    Item->SetStringField(TEXT("name"), Info.Name.ToString());
                    Item->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association));
                    Item->SetNumberField(TEXT("index"), Info.Index);
                    TArray<TSharedPtr<FJsonValue>> Channels;
                    Channels.Add(MakeShared<FJsonValueNumber>(Value.R));
                    Channels.Add(MakeShared<FJsonValueNumber>(Value.G));
                    Channels.Add(MakeShared<FJsonValueNumber>(Value.B));
                    Channels.Add(MakeShared<FJsonValueNumber>(Value.A));
                    Item->SetArrayField(TEXT("value"), Channels);
                    Vectors.Add(MakeShared<FJsonValueObject>(Item));
                }
            }
            Slot->SetArrayField(TEXT("scalar_parameters"), Scalars);
            Slot->SetArrayField(TEXT("vector_parameters"), Vectors);
        }
        Slots.Add(MakeShared<FJsonValueObject>(Slot));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("actor_name"), ActorName);
    ResultData->SetStringField(TEXT("component_name"), TargetComponent->GetName());
    ResultData->SetNumberField(TEXT("material_slots"), NumMaterials);
    ResultData->SetArrayField(TEXT("slots"), Slots);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    return FUnrealMCPCommonUtils::CreateSuccessResponse(MCPMaterialGraphLayout::Describe(Material));
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleListMaterialParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Scalars;
    TArray<TSharedPtr<FJsonValue>> Vectors;
    TArray<TSharedPtr<FJsonValue>> Textures;
    TArray<TSharedPtr<FJsonValue>> Switches;
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        TSharedPtr<FJsonObject> Item = ExpressionPayload(Expression);
        if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            Item->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
            Scalars.Add(MakeShared<FJsonValueObject>(Item));
        }
        else if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
        {
            TArray<TSharedPtr<FJsonValue>> Channels;
            Channels.Add(MakeShared<FJsonValueNumber>(Vector->DefaultValue.R));
            Channels.Add(MakeShared<FJsonValueNumber>(Vector->DefaultValue.G));
            Channels.Add(MakeShared<FJsonValueNumber>(Vector->DefaultValue.B));
            Channels.Add(MakeShared<FJsonValueNumber>(Vector->DefaultValue.A));
            Item->SetArrayField(TEXT("default_value"), Channels);
            Vectors.Add(MakeShared<FJsonValueObject>(Item));
        }
        else if (const UMaterialExpressionTextureSampleParameter* TextureParam = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
        {
            // TextureSampleParameter 的名字在基类上，纹理在 TextureBase 上
            Item->SetStringField(TEXT("parameter_name"), TextureParam->ParameterName.ToString());
            Item->SetStringField(TEXT("default_texture"), TextureParam->Texture ? TextureParam->Texture->GetPathName() : FString());
            Textures.Add(MakeShared<FJsonValueObject>(Item));
        }
        else if (const UMaterialExpressionStaticBoolParameter* Switch = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
        {
            // 静态开关的值在 StaticBoolParameter 基类的 DefaultValue 位域上，不是 Value。
            Item->SetBoolField(TEXT("default_value"), Switch->DefaultValue != 0);
            Item->SetStringField(TEXT("parameter_name"), Switch->ParameterName.ToString());
            Switches.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetArrayField(TEXT("scalar_parameters"), Scalars);
    ResultData->SetArrayField(TEXT("vector_parameters"), Vectors);
    ResultData->SetArrayField(TEXT("texture_parameters"), Textures);
    ResultData->SetArrayField(TEXT("static_switch_parameters"), Switches);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material) return FUnrealMCPCommonUtils::CreateErrorResponse(Error);

    FString ClassName;
    if (!Params->TryGetStringField(TEXT("expression_class"), ClassName))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'expression_class' parameter"));
    UClass* ExpressionClass = FindExpressionClass(ClassName);
    if (!ExpressionClass || ExpressionClass->IsChildOf(UMaterialExpressionComment::StaticClass()))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unknown expression class; use add_material_comment for comments"));

    TStrongObjectPtr<UMaterialExpression> Preview(NewObject<UMaterialExpression>(GetTransientPackage(), ExpressionClass));
    FString ParameterName;
    if (Params->HasField(TEXT("parameter_name")) && !Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("parameter_name must be a string"));
    if (Params->HasField(TEXT("parameter_name")))
    {
        if (!Preview->HasAParameterName() || ParameterName.IsEmpty() || ParameterName.Len() >= NAME_SIZE ||
            !FName::IsValidXName(ParameterName, INVALID_OBJECTNAME_CHARACTERS))
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("parameter_name requires a parameter expression and a nonempty name"));
        Preview->SetParameterName(FName(*ParameterName));
    }
    const TSharedPtr<FJsonObject>* Properties = nullptr;
    TArray<FString> Applied;
    if (Params->HasField(TEXT("properties")))
    {
        if (!Params->TryGetObjectField(TEXT("properties"), Properties) ||
            !ApplyJsonProperties(Preview.Get(), *Properties, Applied, Error))
            return FUnrealMCPCommonUtils::CreateErrorResponse(Error.IsEmpty() ? TEXT("properties must be an object") : Error);
    }
    int32 X = 0, Y = 0;
    for (const TCHAR* Axis : {TEXT("x"), TEXT("y")})
    {
        if (!Params->HasField(Axis)) continue;
        double Value;
        if (!Params->TryGetNumberField(Axis, Value) || !FMath::IsFinite(Value) ||
            Value < MIN_int32 || Value > MAX_int32 || FMath::FloorToDouble(Value) != Value)
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("x and y must be int32 coordinates"));
        (FCString::Strcmp(Axis, TEXT("x")) == 0 ? X : Y) = static_cast<int32>(Value);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddMaterialExpression", "UnrealMCP Add Material Expression"));
    Material->Modify();
    UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, X, Y);
    if (!Expression) return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Expression creation failed"));
    for (const FString& Name : Applied)
        ExpressionClass->FindPropertyByName(*Name)->CopyCompleteValue_InContainer(Expression, Preview.Get());
    if (Preview->HasAParameterName())
    {
        Expression->SetParameterName(Preview->GetParameterName());
        Expression->ValidateParameterName(false);
    }
    if (!Params->HasField(TEXT("x")) && !Params->HasField(TEXT("y"))) PlaceExpressionWithoutOverlap(Material, Expression);
    if (UMaterialExpressionTextureBase* Texture = Cast<UMaterialExpressionTextureBase>(Expression)) Texture->AutoSetSampleType();
    Expression->PostEditChange();
    return FinishGraphEdit(Material, Params, ExpressionPayload(Expression));
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* Expression = RequireExpression(Material, Params, TEXT("node_id"), Error);
    if (!Expression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'properties' object"));
    }
    if ((*PropertiesPtr)->Values.Num() == 0)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("properties must contain at least one field"));

    TStrongObjectPtr<UMaterialExpression> Preview(DuplicateObject<UMaterialExpression>(Expression, GetTransientPackage()));
    TArray<FString> Applied;
    if (!ApplyJsonProperties(Preview.Get(), *PropertiesPtr, Applied, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetMaterialExpressionProperty", "UnrealMCP Set Material Expression Property"));
    Material->Modify();
    Expression->Modify();
    for (const FString& Name : Applied)
    {
        Expression->GetClass()->FindPropertyByName(*Name)->CopyCompleteValue_InContainer(Expression, Preview.Get());
    }

    if (Expression->HasAParameterName())
    {
        if ((*PropertiesPtr)->HasField(TEXT("ParameterName"))) Expression->ValidateParameterName(false);
        Expression->UpdateParameterGuid(false, true);
    }
    if (UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(Expression))
    {
        TextureBase->AutoSetSampleType();
    }
    Expression->PostEditChange();

    TSharedPtr<FJsonObject> ResultData = ExpressionPayload(Expression);
    ResultData->SetArrayField(TEXT("applied"), [&Applied]()
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (const FString& Name : Applied)
        {
            Items.Add(MakeShared<FJsonValueString>(Name));
        }
        return Items;
    }());
    return FinishGraphEdit(Material, Params, ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* Expression = RequireExpression(Material, Params, TEXT("node_id"), Error);
    if (!Expression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    if (!Expression->CanUserDeleteExpression())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("This expression cannot be deleted"));
    }

    const FString DeletedId = NodeIdOf(Expression);
    const FString DeletedClass = Expression->GetClass()->GetName();

    // 先把断开的引用记录下来：删除之后无法再回读，调用方需要知道哪些属性被清空。
    TArray<TSharedPtr<FJsonValue>> BrokenProperties;
    for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
    {
        const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
        FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
        if (Input && Input->Expression == Expression)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("property"), MaterialPropertyName(Property));
            BrokenProperties.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "DeleteMaterialExpression", "UnrealMCP Delete Material Expression"));
    Material->Modify();
    Expression->Modify();
    for (UMaterialExpression* Consumer : Material->Expressions)
    {
        if (Consumer) Consumer->Modify();
    }
    if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expression))
    {
        Material->EditorComments.Remove(Comment);
        Comment->MarkPendingKill();
        Material->MarkPackageDirty();
    }
    else
    {
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
    }
    RecompileAndRefresh(Material);

    FString SaveError;
    if (!SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Expression %s deleted but save failed: %s"), *DeletedId, *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("deleted_node_id"), DeletedId);
    ResultData->SetStringField(TEXT("deleted_class"), DeletedClass);
    ResultData->SetArrayField(TEXT("cleared_property_inputs"), BrokenProperties);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* From = RequireExpression(Material, Params, TEXT("from_node_id"), Error);
    if (!From)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* To = RequireExpression(Material, Params, TEXT("to_node_id"), Error);
    if (!To)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    if (From == To)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Cannot connect an expression to itself"));
    }

    FString OutputName;
    int32 OutputIndex = -1;
    if (!ReadOutputLocator(Params, OutputName, OutputIndex, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    const int32 ResolvedOutput = FindOutputIndex(From, OutputName, OutputIndex);
    if (ResolvedOutput == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Output '%s' does not exist on %s"), *OutputName, *From->GetClass()->GetName()));
    }

    int32 InputIndex = 0;
    FName InputName = NAME_None;
    if (!ReadInputLocator(Params, InputIndex, InputName, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FExpressionInput* TargetInput = FindInput(To, InputIndex, InputName);
    if (!TargetInput)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Input '%s' does not exist on %s"), *InputName.ToString(), *To->GetClass()->GetName()));
    }

    // 定位到真实输入序号：名字定位时调用方给的序号可能是 0，回写索引必须按指针反查。
    int32 ResolvedInputIndex = INDEX_NONE;
    {
        TArray<FExpressionInput*> Inputs = To->GetInputs();
        for (int32 Index = 0; Index < Inputs.Num(); ++Index)
        {
            if (Inputs[Index] == TargetInput)
            {
                ResolvedInputIndex = Index;
                break;
            }
        }
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ConnectMaterialExpressions", "UnrealMCP Connect Material Expressions"));
    Material->Modify();
    To->Modify();
    TargetInput->Connect(ResolvedOutput, From);
    To->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("from_node_id"), NodeIdOf(From));
    ResultData->SetNumberField(TEXT("from_output_index"), ResolvedOutput);
    ResultData->SetStringField(TEXT("to_node_id"), NodeIdOf(To));
    ResultData->SetNumberField(TEXT("to_input_index"), ResolvedInputIndex);
    ResultData->SetStringField(TEXT("to_input"), To->GetInputName(ResolvedInputIndex).ToString());
    return FinishGraphEdit(Material, Params, ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDisconnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* To = RequireExpression(Material, Params, TEXT("to_node_id"), Error);
    if (!To)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    int32 InputIndex = 0;
    FName InputName = NAME_None;
    if (!ReadInputLocator(Params, InputIndex, InputName, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FExpressionInput* TargetInput = FindInput(To, InputIndex, InputName);
    if (!TargetInput)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Input '%s' does not exist on %s"), *InputName.ToString(), *To->GetClass()->GetName()));
    }

    // 可选的 from_node_id 把断开范围收窄到一条线；省略时断开该输入上的所有来源。
    bool bBreakAll = true;
    FString FromNodeId;
    if (Params->TryGetStringField(TEXT("from_node_id"), FromNodeId))
    {
        bBreakAll = false;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "DisconnectMaterialExpressions", "UnrealMCP Disconnect Material Expressions"));
    Material->Modify();
    To->Modify();
    TArray<TSharedPtr<FJsonValue>> Removed;
    if (bBreakAll)
    {
        if (TargetInput->Expression)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("from_node_id"), NodeIdOf(TargetInput->Expression));
            Item->SetNumberField(TEXT("from_output_index"), TargetInput->OutputIndex);
            Removed.Add(MakeShared<FJsonValueObject>(Item));
        }
        TargetInput->Expression = nullptr;
        TargetInput->OutputIndex = 0;
    }
    else
    {
        bool bAmbiguous = false;
        UMaterialExpression* From = FindExpression(Material, FromNodeId, bAmbiguous);
        if (!From)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *FromNodeId));
        }
        if (TargetInput->Expression != From)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("That node is not connected to this input"));
        }
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("from_node_id"), NodeIdOf(From));
        Item->SetNumberField(TEXT("from_output_index"), TargetInput->OutputIndex);
        Removed.Add(MakeShared<FJsonValueObject>(Item));
        TargetInput->Expression = nullptr;
        TargetInput->OutputIndex = 0;
    }
    if (Removed.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Nothing was connected to that input"));
    }
    To->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("to_node_id"), NodeIdOf(To));
    ResultData->SetArrayField(TEXT("removed"), Removed);
    return FinishGraphEdit(Material, Params, ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleConnectMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    UMaterialExpression* From = RequireExpression(Material, Params, TEXT("from_node_id"), Error);
    if (!From)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FString PropertyText;
    if (!Params->TryGetStringField(TEXT("property"), PropertyText))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property' parameter"));
    }
    EMaterialProperty Property = MP_MAX;
    if (!ResolveMaterialProperty(PropertyText, Property, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    FString OutputName;
    int32 OutputIndex = -1;
    if (!ReadOutputLocator(Params, OutputName, OutputIndex, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    const int32 ResolvedOutput = FindOutputIndex(From, OutputName, OutputIndex);
    if (ResolvedOutput == INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Output '%s' does not exist on %s"), *OutputName, *From->GetClass()->GetName()));
    }

    FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
    if (!Input)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("%s is not an input this material can compile"), *PropertyText));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ConnectMaterialProperty", "UnrealMCP Connect Material Property"));
    Material->Modify();
    From->Modify();
    Input->Connect(ResolvedOutput, From);

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("property"), PropertyText);
    ResultData->SetStringField(TEXT("from_node_id"), NodeIdOf(From));
    ResultData->SetNumberField(TEXT("from_output_index"), ResolvedOutput);
    return FinishGraphEdit(Material, Params, ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleDisconnectMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FString PropertyText;
    if (!Params->TryGetStringField(TEXT("property"), PropertyText))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property' parameter"));
    }
    EMaterialProperty Property = MP_MAX;
    if (!ResolveMaterialProperty(PropertyText, Property, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
    if (!Input || !Input->Expression)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Nothing is connected to %s"), *PropertyText));
    }

    const FString PreviousNodeId = NodeIdOf(Input->Expression);
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "DisconnectMaterialProperty", "UnrealMCP Disconnect Material Property"));
    Material->Modify();
    Input->Expression = nullptr;
    Input->OutputIndex = 0;

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("property"), PropertyText);
    ResultData->SetStringField(TEXT("previous_node_id"), PreviousNodeId);
    return FinishGraphEdit(Material, Params, ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    TEnumAsByte<EBlendMode> NewBlendMode;
    TEnumAsByte<EMaterialShadingModel> NewShadingModel;
    TEnumAsByte<EMaterialDomain> NewDomain;
    bool bHasBlendMode = false, bHasShadingModel = false, bHasDomain = false, bHasTwoSided = false, bHasClip = false;
    FString BlendModeText, ShadingModelText, DomainText;
    int64 EnumValue = 0;
    if (Params->HasField(TEXT("blend_mode")))
    {
        if (!Params->TryGetStringField(TEXT("blend_mode"), BlendModeText) ||
            !ResolveEnumValueByName(StaticEnum<EBlendMode>(), BlendModeText, TEXT("BLEND_"), EnumValue) || EnumValue >= BLEND_MAX)
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid blend_mode"));
        NewBlendMode = static_cast<EBlendMode>(EnumValue); bHasBlendMode = true;
    }
    if (Params->HasField(TEXT("shading_model")))
    {
        if (!Params->TryGetStringField(TEXT("shading_model"), ShadingModelText) ||
            !ResolveEnumValueByName(StaticEnum<EMaterialShadingModel>(), ShadingModelText, TEXT("MSM_"), EnumValue) || EnumValue >= MSM_NUM)
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid shading_model"));
        NewShadingModel = static_cast<EMaterialShadingModel>(EnumValue); bHasShadingModel = true;
    }
    if (Params->HasField(TEXT("material_domain")))
    {
        if (!Params->TryGetStringField(TEXT("material_domain"), DomainText) ||
            !ResolveEnumValueByName(StaticEnum<EMaterialDomain>(), DomainText, TEXT("MD_"), EnumValue) || EnumValue >= MD_MAX)
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid material_domain"));
        NewDomain = static_cast<EMaterialDomain>(EnumValue); bHasDomain = true;
    }
    bool bTwoSided = false;
    if (Params->HasField(TEXT("two_sided")))
    {
        if (!Params->TryGetBoolField(TEXT("two_sided"), bTwoSided))
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("two_sided must be a boolean"));
        bHasTwoSided = true;
    }
    double ClipValue = 0.0;
    if (Params->HasField(TEXT("opacity_mask_clip_value")))
    {
        if (!Params->TryGetNumberField(TEXT("opacity_mask_clip_value"), ClipValue) ||
            !FMath::IsFinite(ClipValue) || ClipValue < 0.0 || ClipValue > 1.0)
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("opacity_mask_clip_value must be between 0 and 1"));
        bHasClip = true;
    }
    if (!(bHasBlendMode || bHasShadingModel || bHasDomain || bHasTwoSided || bHasClip))
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide at least one supported material setting"));

    TArray<TSharedPtr<FJsonValue>> Applied;
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetMaterialSettings", "UnrealMCP Set Material Settings"));
    Material->PreEditChange(nullptr);
    Material->Modify();

    if (bHasBlendMode)
    {
        Material->BlendMode = NewBlendMode;
        Applied.Add(MakeShared<FJsonValueString>(TEXT("blend_mode")));
    }

    if (bHasShadingModel)
    {
        Material->SetShadingModel(NewShadingModel);
        Applied.Add(MakeShared<FJsonValueString>(TEXT("shading_model")));
    }

    if (bHasDomain)
    {
        Material->MaterialDomain = NewDomain;
        Applied.Add(MakeShared<FJsonValueString>(TEXT("material_domain")));
    }

    if (bHasTwoSided)
    {
        Material->TwoSided = bTwoSided ? 1 : 0;
        Applied.Add(MakeShared<FJsonValueString>(TEXT("two_sided")));
    }

    if (bHasClip)
    {
        Material->OpacityMaskClipValue = (float)ClipValue;
        Applied.Add(MakeShared<FJsonValueString>(TEXT("opacity_mask_clip_value")));
    }

    Material->PostEditChange();
    RecompileAndRefresh(Material);

    FString SaveError;
    if (!SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Settings applied but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = MCPMaterialGraphLayout::Describe(Material);
    ResultData->SetArrayField(TEXT("applied"), Applied);
    ResultData->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleAddMaterialComment(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FString Text;
    if (!Params->TryGetStringField(TEXT("text"), Text))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'text' parameter"));
    }
    if (Params->HasField(TEXT("color")))
    {
        FLinearColor ValidatedColor;
        FString ColorError;
        if (!ReadLinearColor(Params, TEXT("color"), ValidatedColor, ColorError) || !ColorError.IsEmpty())
            return FUnrealMCPCommonUtils::CreateErrorResponse(ColorError.IsEmpty() ? TEXT("color must contain 3 or 4 channels") : ColorError);
    }
    for (const TCHAR* Field : {TEXT("x"), TEXT("y"), TEXT("size_x"), TEXT("size_y")})
    {
        if (!Params->HasField(Field)) continue;
        double Number;
        const bool bSize = FCString::Strncmp(Field, TEXT("size_"), 5) == 0;
        if (!Params->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number) ||
            FMath::FloorToDouble(Number) != Number || Number < (bSize ? 0 : MIN_int32) || Number > MAX_int32)
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s must be a valid int32 coordinate or size"), Field));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AddMaterialComment", "UnrealMCP Add Material Comment"));
    Material->Modify();
    UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Material, NAME_None, RF_Transactional);
    if (!Comment)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create comment"));
    }
    Comment->Text = Text;
    Comment->Material = Material;

    double Value = 0.0;
    if (Params->TryGetNumberField(TEXT("x"), Value)) Comment->MaterialExpressionEditorX = static_cast<int32>(Value);
    if (Params->TryGetNumberField(TEXT("y"), Value)) Comment->MaterialExpressionEditorY = static_cast<int32>(Value);
    if (Params->TryGetNumberField(TEXT("size_x"), Value)) Comment->SizeX = FMath::Max(0, static_cast<int32>(Value));
    if (Params->TryGetNumberField(TEXT("size_y"), Value)) Comment->SizeY = FMath::Max(0, static_cast<int32>(Value));

    FLinearColor Color;
    FString ColorError;
    if (Params->HasField(TEXT("color")))
    {
        ReadLinearColor(Params, TEXT("color"), Color, ColorError);
        Comment->CommentColor = Color;
    }
    Comment->UpdateMaterialExpressionGuid(true, true);

    Material->EditorComments.Add(Comment);
    Material->PostEditChange();
    Material->MarkPackageDirty();

    FString SaveError;
    if (!SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Comment added but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("material_path"), Material->GetPathName());
    ResultData->SetStringField(TEXT("node_id"), NodeIdOf(Comment));
    ResultData->SetStringField(TEXT("text"), Comment->Text);
    ResultData->SetNumberField(TEXT("x"), Comment->MaterialExpressionEditorX);
    ResultData->SetNumberField(TEXT("y"), Comment->MaterialExpressionEditorY);
    ResultData->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleFormatMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterial* Material = ResolveMaterial(Params, Error);
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    bool bDryRun = false;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    TUniquePtr<FScopedTransaction> Transaction;
    if (!bDryRun)
    {
        Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UnrealMCP", "FormatMaterialGraph", "UnrealMCP Format Material Graph"));
        Material->Modify();
    }
    TSharedPtr<FJsonObject> Result = MCPMaterialGraphLayout::Format(Material, Params);
    if (Result->HasField(TEXT("error")))
    {
        return Result;
    }
    FString SaveError;
    if (!bDryRun && !SaveIfNeeded(Material, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph formatted but save failed: %s"), *SaveError));
    }
    Result->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Result);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString InstanceName;
    if (!Params->TryGetStringField(TEXT("instance_name"), InstanceName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'instance_name' parameter"));
    }
    FString Error;
    UMaterialInterface* Parent = ResolveMaterialInterface(Params, TEXT("parent_material_path"), Error);
    if (!Parent)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    FString SavePath = TEXT("/Game/Materials");
    Params->TryGetStringField(TEXT("save_path"), SavePath);
    SavePath.TrimStartAndEndInline();
    if (SavePath.EndsWith(TEXT("/")))
    {
        SavePath = SavePath.LeftChop(1);
    }
    const FString PackagePath = SavePath + TEXT("/") + InstanceName;
    FString PathError;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(PackagePath, InstanceName, PathError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
    }
    if (UEditorAssetLibrary::DoesAssetExist(PackagePath + TEXT(".") + InstanceName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Material instance already exists: %s"), *PackagePath));
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package for material instance"));
    }
    UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, *InstanceName, RF_Public | RF_Standalone | RF_Transactional);
    if (!Instance)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create material instance object"));
    }

    // 先设父再注册：参数集合由父材质派生，顺序反了实例编辑器会拿到空参数表。
    Instance->SetParentEditorOnly(Parent, false);
    Instance->PostEditChange();
    Instance->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Instance);

    FString SaveError;
    if (!SaveIfNeeded(Instance, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Instance created but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("instance_name"), InstanceName);
    ResultData->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    ResultData->SetStringField(TEXT("parent_material_path"), Parent->GetPathName());
    ResultData->SetStringField(TEXT("package_path"), PackagePath);
    ResultData->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterialInstanceConstant* Instance = ResolveMaterialInstance(Params, Error);
    if (!Instance)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    FString ParamName;
    if (!Params->TryGetStringField(TEXT("param_name"), ParamName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'param_name' parameter"));
    }

    const bool bScalar = Params->HasField(TEXT("scalar"));
    const bool bVector = Params->HasField(TEXT("vector"));
    const bool bTexture = Params->HasField(TEXT("texture"));
    const bool bSwitch = Params->HasField(TEXT("static_switch"));
    const int32 Provided = (bScalar ? 1 : 0) + (bVector ? 1 : 0) + (bTexture ? 1 : 0) + (bSwitch ? 1 : 0);
    if (Provided != 1)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Provide exactly one of scalar, vector, texture or static_switch"));
    }

    UMaterialInterface* Parent = Instance->Parent;
    if (!Parent) return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Material instance has no parent"));
    if (ParamName.IsEmpty() || ParamName.Len() >= NAME_SIZE)
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("param_name is empty or too long"));
    const FName Name(*ParamName);
    const FMaterialParameterInfo ParameterInfo{Name};
    TArray<FName> Names;
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetMaterialInstanceParameter", "UnrealMCP Set Material Instance Parameter"));
    Instance->Modify();

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    ResultData->SetStringField(TEXT("param_name"), ParamName);

    if (bScalar)
    {
        double Value = 0.0;
        if (!Params->TryGetNumberField(TEXT("scalar"), Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'scalar' must be a finite number"));
        }
        Names.Reset();
        UMaterialEditingLibrary::GetScalarParameterNames(Parent, Names);
        if (!Names.Contains(Name))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("Scalar parameter '%s' does not exist on %s"), *ParamName, *Parent->GetPathName()));
        }
        Instance->SetScalarParameterValueEditorOnly(ParameterInfo, (float)Value);
        ResultData->SetStringField(TEXT("kind"), TEXT("scalar"));
        ResultData->SetNumberField(TEXT("value"), Value);
    }
    else if (bVector)
    {
        FLinearColor Color;
        FString ColorError;
        ReadLinearColor(Params, TEXT("vector"), Color, ColorError);
        if (!ColorError.IsEmpty())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(ColorError);
        }
        if (!Params->HasField(TEXT("vector")))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'vector' parameter"));
        }
        Names.Reset();
        UMaterialEditingLibrary::GetVectorParameterNames(Parent, Names);
        if (!Names.Contains(Name))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("Vector parameter '%s' does not exist on %s"), *ParamName, *Parent->GetPathName()));
        }
        Instance->SetVectorParameterValueEditorOnly(ParameterInfo, Color);
        TArray<TSharedPtr<FJsonValue>> Channels;
        Channels.Add(MakeShared<FJsonValueNumber>(Color.R));
        Channels.Add(MakeShared<FJsonValueNumber>(Color.G));
        Channels.Add(MakeShared<FJsonValueNumber>(Color.B));
        Channels.Add(MakeShared<FJsonValueNumber>(Color.A));
        ResultData->SetStringField(TEXT("kind"), TEXT("vector"));
        ResultData->SetArrayField(TEXT("value"), Channels);
    }
    else if (bTexture)
    {
        FString TexturePath;
        if (!Params->TryGetStringField(TEXT("texture"), TexturePath))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'texture' must be an asset path string"));
        }
        Names.Reset();
        UMaterialEditingLibrary::GetTextureParameterNames(Parent, Names);
        if (!Names.Contains(Name))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("Texture parameter '%s' does not exist on %s"), *ParamName, *Parent->GetPathName()));
        }
        UTexture* TextureAsset = TexturePath.IsEmpty() ? nullptr : Cast<UTexture>(LoadAssetByAnyName(TexturePath));
        if (!TexturePath.IsEmpty() && !TextureAsset)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("Failed to load texture: %s"), *TexturePath));
        }
        Instance->SetTextureParameterValueEditorOnly(ParameterInfo, TextureAsset);
        ResultData->SetStringField(TEXT("kind"), TEXT("texture"));
        ResultData->SetStringField(TEXT("value"), TextureAsset ? TextureAsset->GetPathName() : FString());
    }
    else
    {
        bool bValue = false;
        if (!Params->TryGetBoolField(TEXT("static_switch"), bValue))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'static_switch' must be a boolean"));
        }
        FStaticParameterSet StaticParameters;
        Instance->GetStaticParameterValues(StaticParameters);
        FStaticSwitchParameter* Target = nullptr;
        for (FStaticSwitchParameter& Candidate : StaticParameters.StaticSwitchParameters)
        {
            if (Candidate.ParameterInfo.Name == Name)
            {
                Target = &Candidate;
                break;
            }
        }
        if (!Target)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
                TEXT("Static switch parameter '%s' does not exist on %s"), *ParamName, *Parent->GetPathName()));
        }
        // bOverride 打开后静态开关才会作用到实例上，只改值会被父材质覆盖。
        Target->bOverride = true;
        Target->Value = bValue;
        Instance->UpdateStaticPermutation(StaticParameters);
        ResultData->SetStringField(TEXT("kind"), TEXT("static_switch"));
        ResultData->SetBoolField(TEXT("value"), bValue);
    }

    Instance->PostEditChange();
    Instance->MarkPackageDirty();
    UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

    FString SaveError;
    if (!SaveIfNeeded(Instance, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Parameter applied but save failed: %s"), *SaveError));
    }
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}

TSharedPtr<FJsonObject> FUnrealMCPMaterialCommands::HandleGetMaterialInstanceParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UMaterialInstanceConstant* Instance = ResolveMaterialInstance(Params, Error);
    if (!Instance)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    TArray<TSharedPtr<FJsonValue>> Scalars;
    for (const FScalarParameterValue& Entry : Instance->ScalarParameterValues)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
        Item->SetNumberField(TEXT("value"), Entry.ParameterValue);
        Scalars.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> Vectors;
    for (const FVectorParameterValue& Entry : Instance->VectorParameterValues)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
        TArray<TSharedPtr<FJsonValue>> Channels;
        Channels.Add(MakeShared<FJsonValueNumber>(Entry.ParameterValue.R));
        Channels.Add(MakeShared<FJsonValueNumber>(Entry.ParameterValue.G));
        Channels.Add(MakeShared<FJsonValueNumber>(Entry.ParameterValue.B));
        Channels.Add(MakeShared<FJsonValueNumber>(Entry.ParameterValue.A));
        Item->SetArrayField(TEXT("value"), Channels);
        Vectors.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> Textures;
    for (const FTextureParameterValue& Entry : Instance->TextureParameterValues)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
        Item->SetStringField(TEXT("value"), Entry.ParameterValue ? Entry.ParameterValue->GetPathName() : FString());
        Textures.Add(MakeShared<FJsonValueObject>(Item));
    }
    TArray<TSharedPtr<FJsonValue>> Switches;
    FStaticParameterSet StaticParameters;
    Instance->GetStaticParameterValues(StaticParameters);
    for (const FStaticSwitchParameter& Entry : StaticParameters.StaticSwitchParameters)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
        Item->SetBoolField(TEXT("value"), Entry.Value);
        Item->SetBoolField(TEXT("override"), Entry.bOverride);
        Switches.Add(MakeShared<FJsonValueObject>(Item));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    ResultData->SetStringField(TEXT("parent_material_path"), Instance->Parent ? Instance->Parent->GetPathName() : FString());
    ResultData->SetArrayField(TEXT("scalar_parameters"), Scalars);
    ResultData->SetArrayField(TEXT("vector_parameters"), Vectors);
    ResultData->SetArrayField(TEXT("texture_parameters"), Textures);
    ResultData->SetArrayField(TEXT("static_switch_parameters"), Switches);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(ResultData);
}
