#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
// We'll create widgets using regular Factory classes
#include "Factories/Factory.h"
// Remove problematic includes that don't exist in UE 5.5
// #include "UMGEditorSubsystem.h"
// #include "WidgetBlueprintFactory.h"
#include "WidgetBlueprintEditor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/CanvasPanelSlot.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/Button.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Event.h"
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraphSchema_K2_Actions.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

namespace
{
	// 在资产注册表中按资产名查找；调用方用自定义 path 创建的控件也能被后续命令找到。
	FString ResolveWidgetAssetPath(const FString& BlueprintName)
	{
		if (BlueprintName.StartsWith(TEXT("/Game/")))
		{
			return BlueprintName;
		}
		// 先按常用目录快速命中，避免每次全量扫描。
		static const TCHAR* CommonDirectories[] = {
			TEXT("/Game/UI/"),
			TEXT("/Game/Widgets/"),
			TEXT("/Game/Blueprints/")
		};
		for (const TCHAR* Directory : CommonDirectories)
		{
			const FString Candidate = FString(Directory) + BlueprintName;
			if (UEditorAssetLibrary::DoesAssetExist(Candidate))
			{
				return Candidate;
			}
		}
		// 再按资产名全局查找，覆盖任意自定义目录。
		// 同名资产存在多份时必须拒绝，不能静默选一个而修改错对象。
		const FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Matches;
		RegistryModule.Get().GetAssetsByPath(FName(TEXT("/Game")), Matches, true);
		TArray<FString> FoundPaths;
		for (const FAssetData& Asset : Matches)
		{
			if (Asset.AssetName.ToString() != BlueprintName)
			{
				continue;
			}
			const FString Found = Asset.ObjectPath.ToString();
			if (UEditorAssetLibrary::DoesAssetExist(Found))
			{
				FoundPaths.AddUnique(Found);
			}
		}
		if (FoundPaths.Num() == 1)
		{
			return FoundPaths[0];
		}
		if (FoundPaths.Num() > 1)
		{
			UE_LOG(LogTemp, Error, TEXT("UnrealMCP: asset name '%s' is ambiguous; pass a full /Game path. Candidates: %s"),
				*BlueprintName, *FString::Join(FoundPaths, TEXT(", ")));
		}
		return FString();
	}

	// 按名称或完整路径查找 Widget 蓝图；支持旧目录与自定义 path。
	UWidgetBlueprint* FindWidgetBlueprintByPathOrName(const FString& BlueprintName)
	{
		const FString AssetPath = ResolveWidgetAssetPath(BlueprintName);
		return AssetPath.IsEmpty() ? nullptr : Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
	}

	// 客户端把部分参数放在新字段名上（widget_name 指蓝图，子控件用 text_block_name 等）；
	// 主字段优先，缺失时回退到备用字段名，以同时支持新旧调用约定。
	bool TryGetStringFieldAlias(const TSharedPtr<FJsonObject>& Params, const TCHAR* Primary, const TCHAR* Alias, FString& OutValue)
	{
		if (Params->TryGetStringField(Primary, OutValue))
		{
			return true;
		}
		return Params->TryGetStringField(Alias, OutValue);
	}

	FVector2D GetOptionalSize(const TSharedPtr<FJsonObject>& Params, const FVector2D& Default)
	{
		// 未提供 size 时保留默认值；显式传入的尺寸（包括零值）必须原样生效。
		if (!Params->HasField(TEXT("size")))
		{
			return Default;
		}
		return FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("size"));
	}

	FLinearColor GetOptionalColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FLinearColor& Default)
	{
		TArray<float> Values;
		FUnrealMCPCommonUtils::GetFloatArrayFromJson(Params, FieldName, Values);
		if (Values.Num() >= 3)
		{
			return FLinearColor(Values[0], Values[1], Values[2], Values.Num() >= 4 ? Values[3] : 1.0f);
		}
		return Default;
	}

	float GetOptionalFontSize(const TSharedPtr<FJsonObject>& Params, float Default)
	{
		int32 FontSize = 0;
		if (Params->TryGetNumberField(TEXT("font_size"), FontSize))
		{
			return FontSize;
		}
		return Default;
	}

	FString GetWidgetAssetPath(const FString& BlueprintName)
	{
		const FString Resolved = ResolveWidgetAssetPath(BlueprintName);
		return Resolved.IsEmpty() ? (TEXT("/Game/Widgets/") + BlueprintName) : Resolved;
	}
}

UWidgetBlueprint* FUnrealMCPUMGCommands::FindLayoutWidgetBlueprint(const FString& BlueprintName)
{
    return FindWidgetBlueprintByPathOrName(BlueprintName);
}

FUnrealMCPUMGCommands::FUnrealMCPUMGCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCommand(const FString& CommandName, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandName == TEXT("list_widget_blueprints"))
    {
        return HandleListWidgetBlueprints(Params);
    }
    if (CommandName == TEXT("get_widget_tree"))
    {
        return HandleGetWidgetTree(Params);
    }
    if (CommandName == TEXT("add_image_to_widget"))
    {
        return HandleAddImageToWidget(Params);
    }
    if (CommandName == TEXT("add_widget_to_widget"))
    {
        return HandleAddWidgetToWidget(Params);
    }
    if (CommandName == TEXT("set_widget_layout"))
    {
        return HandleSetWidgetLayout(Params);
    }
	if (CommandName == TEXT("create_umg_widget_blueprint"))
	{
		return HandleCreateUMGWidgetBlueprint(Params);
	}
	else if (CommandName == TEXT("add_text_block_to_widget"))
	{
		return HandleAddTextBlockToWidget(Params);
	}
	else if (CommandName == TEXT("add_widget_to_viewport"))
	{
		return HandleAddWidgetToViewport(Params);
	}
	else if (CommandName == TEXT("add_button_to_widget"))
	{
		return HandleAddButtonToWidget(Params);
	}
	else if (CommandName == TEXT("bind_widget_event"))
	{
		return HandleBindWidgetEvent(Params);
	}
	else if (CommandName == TEXT("set_text_block_binding"))
	{
		return HandleSetTextBlockBinding(Params);
	}

	return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown UMG command: %s"), *CommandName));
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("name"), BlueprintName))
	{
		// 兼容 Python 端转发的 widget_name 字段。
		if (!Params->TryGetStringField(TEXT("widget_name"), BlueprintName))
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
		}
	}

	// 支持调用方指定目录；缺省仍用旧路径，保持既有调用兼容。
	FString PackagePath = TEXT("/Game/Widgets/");
	Params->TryGetStringField(TEXT("path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game/")))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("path must start with /Game/"));
	}
	if (!PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath += TEXT("/");
	}
	FString AssetName = BlueprintName;
	FString FullPath = PackagePath + AssetName;
	FString PathError;
	if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(FullPath, AssetName, PathError))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(PathError);
	}

	// Check if asset already exists
	if (UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' already exists"), *BlueprintName));
	}

	// Create package
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package"));
	}

	// 支持调用方指定父类（例如项目 C++ Widget 基类）；缺省仍为 UUserWidget。
	UClass* ParentClass = UUserWidget::StaticClass();
	FString ParentClassName;
	if (Params->TryGetStringField(TEXT("parent_class"), ParentClassName) && !ParentClassName.IsEmpty())
	{
		UClass* Resolved = nullptr;
		if (ParentClassName.StartsWith(TEXT("/Game/")) || ParentClassName.StartsWith(TEXT("/Script/")))
		{
			Resolved = LoadClass<UObject>(nullptr, *ParentClassName);
		}
		if (!Resolved)
		{
			Resolved = FindObject<UClass>(ANY_PACKAGE, *ParentClassName);
		}
		if (!Resolved && !ParentClassName.StartsWith(TEXT("U")))
		{
			Resolved = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + ParentClassName));
		}
		if (!Resolved)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget parent class not found"));
		}
		if (!Resolved->IsChildOf(UUserWidget::StaticClass()))
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("parent_class '%s' is not a UserWidget subclass"), *ParentClassName));
		}
		ParentClass = Resolved;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Parent class does not support Widget Blueprint creation"));
	}
	// UE4.26 的 Widget 蓝图必须用 UWidgetBlueprint 作为 Blueprint 类，
	// 否则生成的资产 Cast<UWidgetBlueprint> 失败，并污染资产注册表。
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,                 // Parent class
		Package,                     // Outer package
		FName(*AssetName),           // Blueprint name
		BPTYPE_Normal,               // Blueprint type
		UWidgetBlueprint::StaticClass(),   // Blueprint class
		UWidgetBlueprintGeneratedClass::StaticClass(), // Generated class
		FName("CreateUMGWidget")     // Creation method name
	);

	// Make sure the Blueprint was created successfully
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewBlueprint);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Widget Blueprint"));
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
	}
	// 没有根控件时创建默认 Canvas Panel。
	if (!WidgetBlueprint->WidgetTree->RootWidget)
	{
		// CreateBlueprint 不经过 PostLoad，源树残留的 RF_ArchetypeObject 会
		// 沿 Outer 链传播，让后续新控件被 IsTemplate 判定为模板；与引擎
		// UWidgetBlueprint::PostLoad 行为保持一致，这里创建前先清除。
		WidgetBlueprint->WidgetTree->ClearFlags(RF_ArchetypeObject);
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
	}

	// Mark the package dirty and notify asset registry
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(WidgetBlueprint);

	// Compile the blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	if (WidgetBlueprint->Status == BS_Error)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint compilation failed"));
	}
	// 保存到磁盘；没有这一步后续 LoadAsset 全部失败。
	FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(WidgetBlueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Widget Blueprint created but save failed"), *MCPAssetSaveError));
                }

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("name"), BlueprintName);
	ResultObj->SetStringField(TEXT("path"), FullPath);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
	}

	FString WidgetName;
	if (!TryGetStringFieldAlias(Params, TEXT("text_block_name"), TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
	}

	// Find the Widget Blueprint
	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Get optional parameters
	FString InitialText = TEXT("New Text Block");
	Params->TryGetStringField(TEXT("text"), InitialText);

	FVector2D Position(0.0f, 0.0f);
	if (Params->HasField(TEXT("position")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArray;
		if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
		{
			Position.X = (*PosArray)[0]->AsNumber();
			Position.Y = (*PosArray)[1]->AsNumber();
		}
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
	}
	FString NameError;
	if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(TEXT("/Game/Widgets/") + WidgetName, WidgetName, NameError))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(NameError);
	}
	if (WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget name already exists"));
	}
	// 创建 TextBlock 模板控件。
	UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *WidgetName);
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Text Block widget"));
	}

	// Set initial text
	TextBlock->SetText(FText::FromString(InitialText));

	// Add to canvas panel
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Root Canvas Panel not found"));
	}

	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(TextBlock);
	if (!PanelSlot)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add TextBlock to canvas"));
	}
	PanelSlot->SetPosition(Position);
	// 客户端声明的尺寸、字号和颜色必须生效，否则调用方会得到静默无效的参数。
	PanelSlot->SetAutoSize(false);
	PanelSlot->SetSize(GetOptionalSize(Params, FVector2D(200.0f, 50.0f)));
	FSlateFontInfo BlockFont = TextBlock->Font;
	BlockFont.Size = (int32)GetOptionalFontSize(Params, BlockFont.Size);
	TextBlock->SetFont(BlockFont);
	TextBlock->SetColorAndOpacity(GetOptionalColor(Params, TEXT("color"), FLinearColor::White));

	// Mark the package dirty and compile
	WidgetBlueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	if (WidgetBlueprint->Status == BS_Error)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Text Block added but Blueprint compilation failed"));
	}
	// 保存到磁盘；否则后续按路径加载会失败。
	FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(WidgetBlueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Text Block added but save failed"), *MCPAssetSaveError));
                }

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("text"), InitialText);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    UWidgetBlueprint* Blueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint not found"));
    }
    if (!Blueprint->GeneratedClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint has no generated class"));
    }
    if (!GEditor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Editor unavailable"));
    }
    UWorld* World = GEditor->PlayWorld;
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("An active PIE session is required"));
    }
    APlayerController* Player = World->GetFirstPlayerController();
    if (!Player)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("PIE player controller unavailable"));
    }
    if (!Player->IsLocalController())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("A local PIE player is required"));
    }
    int32 ZOrder = 0;
    Params->TryGetNumberField(TEXT("z_order"), ZOrder);
    UWidgetBlueprint* WidgetBPLoaded = Cast<UWidgetBlueprint>(Blueprint);
    if (!WidgetBPLoaded)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Target blueprint is not a Widget Blueprint"));
    }
    UUserWidget* Widget = CreateWidget<UUserWidget>(Player, *WidgetBPLoaded->GeneratedClass);
    if (!Widget)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create widget instance"));
    }
    Widget->AddToViewport(ZOrder);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetStringField(TEXT("class_path"), Blueprint->GeneratedClass->GetPathName());
    Result->SetNumberField(TEXT("z_order"), ZOrder);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Result);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, WidgetName, ButtonText;
    if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    if (!TryGetStringFieldAlias(Params, TEXT("button_name"), TEXT("widget_name"), WidgetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("text"), ButtonText))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'text' parameter"));
    }
    UWidgetBlueprint* Blueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint not found"));
    }
    if (!Blueprint->WidgetTree)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
    }
    FString NameError;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(TEXT("/Game/Widgets/") + WidgetName, WidgetName, NameError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(NameError);
    }
    const FString TextWidgetName = WidgetName + TEXT("_Text");
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(TEXT("/Game/Widgets/") + TextWidgetName, TextWidgetName, NameError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(NameError);
    }
    const FName TextName(*TextWidgetName);
    if (Blueprint->WidgetTree->FindWidget(FName(*WidgetName)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget name already exists"));
    }
    if (Blueprint->WidgetTree->FindWidget(TextName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Button text widget name already exists"));
    }
    UCanvasPanel* Canvas = Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget);
    if (!Canvas)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Root widget is not a Canvas Panel"));
    }
    // 模板控件必须属于 WidgetTree，不能挂在生成类 CDO 或其他控件上。
    UButton* Button = Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), FName(*WidgetName));
    if (!Button)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create button template"));
    }
    UTextBlock* Text = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TextName);
    if (!Text)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create button text template"));
    }
    Text->SetText(FText::FromString(ButtonText));
    Button->AddChild(Text);
    Button->bIsVariable = true;
    UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Button);
    if (!Slot)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add button to canvas"));
    }
    Slot->SetPosition(FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("position")));
    // 应用客户端声明的尺寸、字号、文字颜色和背景色。
    Slot->SetAutoSize(false);
    Slot->SetSize(GetOptionalSize(Params, FVector2D(200.0f, 50.0f)));
    FSlateFontInfo ButtonFont = Text->Font;
    ButtonFont.Size = (int32)GetOptionalFontSize(Params, ButtonFont.Size);
    Text->SetFont(ButtonFont);
    Text->SetColorAndOpacity(GetOptionalColor(Params, TEXT("color"), FLinearColor::White));
    Button->SetBackgroundColor(GetOptionalColor(Params, TEXT("background_color"), FLinearColor(0.1f, 0.1f, 0.1f, 1.0f)));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Button added but Blueprint compilation failed"));
    }
    FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(Blueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Button added but save failed"), *MCPAssetSaveError));
                }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget_name"), WidgetName);
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, WidgetName, EventName;
    if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    if (!TryGetStringFieldAlias(Params, TEXT("widget_component_name"), TEXT("widget_name"), WidgetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter"));
    }
    UWidgetBlueprint* Blueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint not found"));
    }
    if (!Blueprint->WidgetTree)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
    }
    UWidget* Widget = Blueprint->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget not found"));
    }
    FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), FName(*EventName));
    if (!Delegate)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget event delegate not found"));
    }
    if (!Widget->bIsVariable)
    {
        Widget->bIsVariable = true;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }
    if (!Blueprint->SkeletonGeneratedClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint has no skeleton class"));
    }
    FObjectProperty* WidgetProperty = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, FName(*WidgetName));
    if (!WidgetProperty)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget variable property not found"));
    }
    if (!FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*EventName), FName(*WidgetName)))
    {
        UEdGraph* Graph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
        if (!Graph)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event graph unavailable"));
        }
        // 使用实际控件属性初始化事件，保证同类控件的事件绑定相互独立。
        FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_ComponentBoundEvent>(Graph, Graph->GetGoodPlaceForNewNode(), EK2NewNodeFlags::None,
            [WidgetProperty, Delegate](UK2Node_ComponentBoundEvent* Node)
            {
                Node->InitializeComponentBoundEventParams(WidgetProperty, Delegate);
            });
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget event added but Blueprint compilation failed"));
    }
    FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(Blueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Widget event added but save failed"), *MCPAssetSaveError));
                }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("event_name"), EventName);
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, WidgetName, BindingName;
    if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    if (!TryGetStringFieldAlias(Params, TEXT("text_block_name"), TEXT("widget_name"), WidgetName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
    }
    if (!TryGetStringFieldAlias(Params, TEXT("binding_property"), TEXT("binding_name"), BindingName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'binding_name' parameter"));
    }
    // 目前只支持绑定 Text 属性；其他类型必须明确报错而不是静默按 Text 处理。
    FString BindingType = TEXT("Text");
    Params->TryGetStringField(TEXT("binding_type"), BindingType);
    if (!BindingType.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported binding_type: %s"), *BindingType));
    }
    UWidgetBlueprint* Blueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint not found"));
    }
    if (!Blueprint->WidgetTree)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
    }
    UTextBlock* Text = Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(FName(*WidgetName)));
    if (!Text)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("TextBlock not found"));
    }
    FString NameError;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(TEXT("/Game/Widgets/") + BindingName, BindingName, NameError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(NameError);
    }
    if (!Blueprint->SkeletonGeneratedClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget Blueprint has no skeleton class"));
    }
    FProperty* Source = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, FName(*BindingName));
    if (!Source)
    {
        if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*BindingName),
            FEdGraphPinType(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType())))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add text binding variable"));
        }
        Source = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, FName(*BindingName));
    }
    if (!CastField<FTextProperty>(Source))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Binding variable must be a Text property"));
    }
    // 直接使用 UE 原生属性绑定，避免生成无返回值且没有注册的冗余函数图。
    FDelegateEditorBinding Binding;
    Binding.ObjectName = Text->GetName();
    Binding.PropertyName = TEXT("Text");
    Binding.SourceProperty = Source->GetFName();
    Binding.Kind = EBindingKind::Property;
    Binding.SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{ FFieldVariant(Source) });
    UBlueprint::GetGuidFromClassByFieldName<FProperty>(Blueprint->SkeletonGeneratedClass, Source->GetFName(), Binding.MemberGuid);
    Blueprint->Bindings.Remove(Binding);
    Blueprint->Bindings.Add(Binding);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Text binding added but Blueprint compilation failed"));
    }
    FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(Blueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Text binding added but save failed"), *MCPAssetSaveError));
                }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("binding_name"), BindingName);
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddImageToWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintName;
	if (!TryGetStringFieldAlias(Params, TEXT("blueprint_name"), TEXT("widget_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
	}

	FString WidgetName;
	if (!TryGetStringFieldAlias(Params, TEXT("image_name"), TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'image_name' parameter"));
	}

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByPathOrName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	FVector2D Position(0.0f, 0.0f);
	const TArray<TSharedPtr<FJsonValue>>* PosArray;
	if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
	{
		Position.X = (*PosArray)[0]->AsNumber();
		Position.Y = (*PosArray)[1]->AsNumber();
	}

	if (!WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget tree unavailable"));
	}
	UImage* Image = nullptr;
	UCanvasPanelSlot* ExistingSlot = nullptr;
	if (UWidget* Existing = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)))
	{
		// 已存在同名控件时改为更新槽位，支持调用方调整位置尺寸。
		Image = Cast<UImage>(Existing);
		ExistingSlot = Cast<UCanvasPanelSlot>(Existing->Slot);
		if (!Image || !ExistingSlot)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Widget name already exists and is not an Image on canvas"));
		}
	}
	if (!Image)
	{
		Image = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), *WidgetName);
		if (!Image)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Image widget"));
		}
	}

	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Root Canvas Panel not found"));
	}

	UCanvasPanelSlot* PanelSlot = ExistingSlot;
	if (!PanelSlot)
	{
		PanelSlot = RootCanvas->AddChildToCanvas(Image);
		if (!PanelSlot)
		{
			return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add Image to canvas"));
		}
	}
	PanelSlot->SetPosition(Position);
	PanelSlot->SetAutoSize(false);
	PanelSlot->SetSize(GetOptionalSize(Params, FVector2D(64.0f, 64.0f)));
	// 图标不拦截鼠标，避免挡住下方交互控件；bHidden 用于静态占位图，待策划配纹理后再改为可见。
	Image->SetVisibility(Params->HasField(TEXT("bHidden")) && Params->GetBoolField(TEXT("bHidden"))
		? ESlateVisibility::Hidden
		: ESlateVisibility::HitTestInvisible);

	WidgetBlueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	if (WidgetBlueprint->Status == BS_Error)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Image added but Blueprint compilation failed"));
	}
	FString MCPAssetSaveError;
                if (!FUnrealMCPCommonUtils::SaveLoadedAssetReliable(WidgetBlueprint, MCPAssetSaveError))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s: %s"), TEXT("Image added but save failed"), *MCPAssetSaveError));
                }

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetNumberField(TEXT("pos_x"), Position.X);
	ResultObj->SetNumberField(TEXT("pos_y"), Position.Y);
	return ResultObj;
}
