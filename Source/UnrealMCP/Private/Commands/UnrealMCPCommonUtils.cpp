#include "Commands/UnrealMCPCommonUtils.h"
#include "UObject/NoExportTypes.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace UnrealMCPSetHelper
{
	// FSetProperty::ElementProp 是 private，经 FScriptSetHelper 的公开读口取元素属性。
	FORCEINLINE FProperty* GetElementProperty(const FSetProperty* SetProp)
	{
		return FScriptSetHelper(SetProp, nullptr).GetElementProperty();
	}
}

// JSON Utilities
TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);

    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }

    return ResponseObject;
}

void FUnrealMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();

    if (!JsonObject->HasField(FieldName))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FUnrealMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();

    if (!JsonObject->HasField(FieldName))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

FVector2D FUnrealMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);

    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }

    return Result;
}

FVector FUnrealMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);

    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }

    return Result;
}

FRotator FUnrealMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);

    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }

    return Result;
}

bool FUnrealMCPCommonUtils::ValidateAssetCreationPath(const FString& PackagePath, const FString& AssetName, FString& OutError)
{
    // UE4 的 IsValidLongPackageName 不拒绝双斜杠，但 CreatePackage 会触发 Fatal。
    const bool bValidPathShape = PackagePath.Len() < NAME_SIZE && !PackagePath.Contains(TEXT("//"));
    const bool bValidPackage = bValidPathShape && PackagePath.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(PackagePath);
    const bool bValidLength = !AssetName.IsEmpty() && AssetName.Len() < NAME_SIZE;
    const bool bValidName = bValidLength && !AssetName.Equals(TEXT("None"), ESearchCase::IgnoreCase) && FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS);
    const bool bValidObject = bValidName && FPackageName::IsValidObjectPath(PackagePath + TEXT(".") + AssetName);
    if (!bValidPackage || !bValidObject)
    {
        OutError = FString::Printf(TEXT("Invalid asset package or name: %s.%s"), *PackagePath, *AssetName);
        return false;
    }
    return true;
}

// Blueprint Utilities
UBlueprint* FUnrealMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FUnrealMCPCommonUtils::FindBlueprintByName(const FString& BlueprintName)
{
    // 传入完整路径时直接加载；否则依次尝试旧默认路径和项目常用目录。
    if (BlueprintName.StartsWith(TEXT("/Game/")))
    {
        return LoadObject<UBlueprint>(nullptr, *BlueprintName);
    }

    // 短名回退：先查通用蓝图目录，再通过资产注册表按名全局查找，
    // 这样无论蓝图放在哪个目录都能被后续命令找到（与 UMG 的解析策略一致）。
    if (UBlueprint* Found = LoadObject<UBlueprint>(nullptr, *(TEXT("/Game/Blueprints/") + BlueprintName)))
    {
        return Found;
    }
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
        const FString ObjectPath = Asset.ObjectPath.ToString();
        if (Cast<UBlueprint>(Asset.GetAsset()))
        {
            FoundPaths.AddUnique(ObjectPath);
        }
    }
    // 同名蓝图存在多份时必须拒绝，避免静默选错对象。
    if (FoundPaths.Num() == 1)
    {
        return LoadObject<UBlueprint>(nullptr, *FoundPaths[0]);
    }
    if (FoundPaths.Num() > 1)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCP: blueprint name '%s' is ambiguous; pass a full /Game path. Candidates: %s"),
            *BlueprintName, *FString::Join(FoundPaths, TEXT(", ")));
    }
    return nullptr;
}

UEdGraph* FUnrealMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    // Use the first existing event page, including renamed Event Graphs.
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph) { return Graph; }
    }

    // Create a new event graph if none exists
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    return NewGraph;
}

UEdGraph* FUnrealMCPCommonUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint || GraphName.IsEmpty())
    {
        return nullptr;
    }

    // 同名图可能跨 Ubergraph / Function / Macro 三类，先收集后判唯一，避免静默选错。
    TArray<UEdGraph*> Candidates;
    auto Collect = [&Candidates, &GraphName](const TArray<UEdGraph*>& Graphs)
    {
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
            {
                Candidates.AddUnique(Graph);
            }
        }
    };
    Collect(Blueprint->UbergraphPages);
    Collect(Blueprint->FunctionGraphs);
    Collect(Blueprint->MacroGraphs);

    if (Candidates.Num() > 1)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCP: graph name '%s' is ambiguous on blueprint '%s'."), *GraphName, *Blueprint->GetName());
        return nullptr;
    }
    return Candidates.Num() == 1 ? Candidates[0] : nullptr;
}

UEdGraph* FUnrealMCPCommonUtils::ResolveGraphForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError,
                                                       bool bAllowCreate)
{
    if (!Blueprint)
    {
        OutError = TEXT("Blueprint unavailable");
        return nullptr;
    }

    FString GraphName;
    if (!Params || !Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
    {
        // 缺省保持旧行为：第一个 Event Graph（写命令可创建，读命令只查）。
        UEdGraph* EventGraph = bAllowCreate ? FindOrCreateEventGraph(Blueprint) : nullptr;
        if (!EventGraph && !bAllowCreate)
        {
            for (UEdGraph* Page : Blueprint->UbergraphPages)
            {
                if (Page)
                {
                    EventGraph = Page;
                    break;
                }
            }
            if (!EventGraph && Blueprint->UbergraphPages.Num() > 0)
            {
                EventGraph = Blueprint->UbergraphPages[0];
            }
        }
        if (!EventGraph)
        {
            OutError = bAllowCreate ? TEXT("Failed to get event graph") : TEXT("Blueprint has no Event Graph");
        }
        return EventGraph;
    }

    UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
    if (!Graph)
    {
        // 候选列表把图作用域的名称空间回传给调用方，避免客户端靠猜。
        TArray<FString> Available;
        for (const TArray<UEdGraph*>* Pages : { &Blueprint->UbergraphPages, &Blueprint->FunctionGraphs, &Blueprint->MacroGraphs })
        {
            for (UEdGraph* Page : *Pages)
            {
                if (Page)
                {
                    Available.AddUnique(Page->GetName());
                }
            }
        }
        OutError = FString::Printf(TEXT("Graph not found or ambiguous: '%s'. Available graphs: %s"),
            *GraphName, Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("<none>"));
        return nullptr;
    }
    return Graph;
}

// Blueprint node utilities
UK2Node_Event* FUnrealMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }

    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }

    // Check for existing event node with this exact name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Using existing event node with name %s (ID: %s)"),
                *EventName, *EventNode->NodeGuid.ToString());
            return EventNode;
        }
    }

    // No existing node found, create a new one
    UK2Node_Event* EventNode = nullptr;

    // Find the function to create the event
    UClass* BlueprintClass = Blueprint->GeneratedClass;
    if (!BlueprintClass)
    {
        BlueprintClass = Blueprint->ParentClass;
    }
    if (!BlueprintClass)
    {
        return nullptr;
    }
    UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));

    if (EventFunction)
    {
        EventNode = NewObject<UK2Node_Event>(Graph);
        // 覆写父类 BlueprintImplementableEvent 时必须设置 bOverrideFunction，
        // 否则只是普通自定义事件，无法接收 C++ 的事件调用。
        EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
        EventNode->bOverrideFunction = true;
        EventNode->NodePosX = Position.X;
        EventNode->NodePosY = Position.Y;
        Graph->AddNode(EventNode, true);
        EventNode->CreateNewGuid();
        EventNode->PostPlacedNewNode();
        EventNode->AllocateDefaultPins();
        EventNode->ReconstructNode();
        UE_LOG(LogTemp, Display, TEXT("Created new event node with name %s (ID: %s)"),
            *EventName, *EventNode->NodeGuid.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to find function for event name: %s"), *EventName);
    }

    return EventNode;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    if (!Function)
    {
        return nullptr;
    }

    UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
    FunctionNode->SetFromFunction(Function);
    FunctionNode->NodePosX = Position.X;
    FunctionNode->NodePosY = Position.Y;
    Graph->AddNode(FunctionNode, true);
    FunctionNode->CreateNewGuid();
    FunctionNode->PostPlacedNewNode();
    FunctionNode->AllocateDefaultPins();

    return FunctionNode;
}

UK2Node_VariableGet* FUnrealMCPCommonUtils::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    if (!Blueprint)
    {
        return nullptr;
    }
    if (!Blueprint->GeneratedClass)
    {
        return nullptr;
    }

    UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);

    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);

    if (Property)
    {
        VariableGetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableGetNode->NodePosX = Position.X;
        VariableGetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableGetNode, true);
        VariableGetNode->PostPlacedNewNode();
        VariableGetNode->AllocateDefaultPins();

        return VariableGetNode;
    }

    return nullptr;
}

UK2Node_VariableSet* FUnrealMCPCommonUtils::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    if (!Blueprint)
    {
        return nullptr;
    }
    if (!Blueprint->GeneratedClass)
    {
        return nullptr;
    }

    UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);

    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);

    if (Property)
    {
        VariableSetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableSetNode->NodePosX = Position.X;
        VariableSetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableSetNode, true);
        VariableSetNode->PostPlacedNewNode();
        VariableSetNode->AllocateDefaultPins();

        return VariableSetNode;
    }

    return nullptr;
}

UK2Node_InputAction* FUnrealMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
    InputActionNode->InputActionName = FName(*ActionName);
    InputActionNode->NodePosX = Position.X;
    InputActionNode->NodePosY = Position.Y;
    Graph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();

    return InputActionNode;
}

UK2Node_Self* FUnrealMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }

    UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
    SelfNode->NodePosX = Position.X;
    SelfNode->NodePosY = Position.Y;
    Graph->AddNode(SelfNode, true);
    SelfNode->CreateNewGuid();
    SelfNode->PostPlacedNewNode();
    SelfNode->AllocateDefaultPins();

    return SelfNode;
}

bool FUnrealMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph)
    {
        return false;
    }
    if (!SourceNode)
    {
        return false;
    }
    if (!TargetNode)
    {
        return false;
    }
    if (SourceNode->GetGraph() != Graph || TargetNode->GetGraph() != Graph)
    {
        return false;
    }

    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);

    if (SourcePin && TargetPin)
    {
        const UEdGraphSchema* Schema = Graph->GetSchema();
        if (!Schema)
        {
            return false;
        }
        return Schema->TryCreateConnection(SourcePin, TargetPin);
    }

    return false;
}

bool FUnrealMCPCommonUtils::DisconnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph || !SourceNode || !TargetNode)
    {
        return false;
    }
    if (SourceNode->GetGraph() != Graph || TargetNode->GetGraph() != Graph)
    {
        return false;
    }

    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    if (!SourcePin || !TargetPin || !SourcePin->LinkedTo.Contains(TargetPin))
    {
        return false;
    }

    // 只断开指定的一对引脚，其余连线保持，这是排版/重构不能违背的约束。
    // 走 schema 才会触发连带清理与变更通知，直接 BreakLinkTo 会绕过它们。
    const UEdGraphSchema* Schema = Graph->GetSchema();
    if (!Schema)
    {
        return false;
    }
    Schema->BreakSinglePinLink(SourcePin, TargetPin);
    return !SourcePin->LinkedTo.Contains(TargetPin);
}

UEdGraphNode* FUnrealMCPCommonUtils::FindNodeByGuid(UEdGraph* Graph, const FString& NodeId)
{
    if (!Graph || NodeId.IsEmpty())
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString() == NodeId)
        {
            return Node;
        }
    }
    return nullptr;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    // Log all pins for debugging
    UE_LOG(LogTemp, Display, TEXT("FindPin: Looking for pin '%s' (Direction: %d) in node '%s'"),
           *PinName, (int32)Direction, *Node->GetName());

    for (UEdGraphPin* Pin : Node->Pins)
    {
        UE_LOG(LogTemp, Display, TEXT("  - Available pin: '%s', Direction: %d, Category: %s"),
               *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
    }

    // First try exact match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found exact matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }

    // If no exact match and we're looking for a component reference, try case-insensitive match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) &&
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }

    // If we're looking for a component output and didn't find it by name, try to find the first data output pin
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                UE_LOG(LogTemp, Display, TEXT("  - Found fallback data output pin: '%s'"), *Pin->PinName.ToString());
                return Pin;
            }
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("  - No matching pin found for '%s'"), *PinName);
    return nullptr;
}

// Actor utilities
TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }

    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);

    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);

    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);

    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ActorToJsonObject(AActor* Actor, bool bDetailed)
{
    if (!Actor)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);

    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);

    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);

    // When bDetailed is true, also enumerate EditAnywhere/BlueprintReadWrite UPROPERTY
    // fields via reflection so the AI can read actor state (Team, OwningTeam, CaptureProgress,
    // MatchDuration, DefaultGameMode, etc.) — not just the transform.
    if (bDetailed)
    {
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        UClass* ActorClass = Actor->GetClass();
        for (TFieldIterator<FProperty> It(ActorClass); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }

            const FString PropName = Prop->GetName();
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);

            // Try common property types. Unsupported types are silently skipped.
            // UE 4.27 uses F*Property (FByteProperty, FInt32Property, etc.).
            if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
            {
                PropsObj->SetNumberField(PropName, (int32)ByteProp->GetPropertyValue(ValuePtr));
            }
            else if (FIntProperty* Int32Prop = CastField<FIntProperty>(Prop))
            {
                PropsObj->SetNumberField(PropName, Int32Prop->GetPropertyValue(ValuePtr));
            }
            else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
            {
                PropsObj->SetNumberField(PropName, FloatProp->GetPropertyValue(ValuePtr));
            }
            else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
            {
                PropsObj->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
            }
            else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
            {
                PropsObj->SetStringField(PropName, StrProp->GetPropertyValue(ValuePtr));
            }
            else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
            {
                PropsObj->SetStringField(PropName, NameProp->GetPropertyValue(ValuePtr).ToString());
            }
            else if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
            {
                UClass* ClassVal = Cast<UClass>(ClassProp->GetPropertyValue(ValuePtr));
                PropsObj->SetStringField(PropName, ClassVal ? ClassVal->GetPathName() : TEXT("None"));
            }
            else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
            {
                UObject* ObjVal = ObjProp->GetPropertyValue(ValuePtr);
                PropsObj->SetStringField(PropName, ObjVal ? ObjVal->GetPathName() : TEXT("None"));
            }
            // Vector/Struct/Array properties are skipped for brevity — AI can use
            // the existing location/rotation/scale fields above for spatial state.
        }
        ActorObject->SetObjectField(TEXT("properties"), PropsObj);
    }

    return ActorObject;
}

UK2Node_Event* FUnrealMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    // Look for existing event nodes
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Found existing event node with name: %s"), *EventName);
            return EventNode;
        }
    }

    return nullptr;
}

bool FUnrealMCPCommonUtils::SetObjectProperty(void* Object, const FString& PropertyName,
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    if (!Value.IsValid())
    {
        OutErrorMessage = TEXT("Invalid JSON property value");
        return false;
    }
    if (PropertyName.IsEmpty() || PropertyName.Len() >= NAME_SIZE)
    {
        OutErrorMessage = TEXT("Property name must be nonempty and shorter than NAME_SIZE");
        return false;
    }
    FProperty* Property = nullptr;
    if (UObject* ObjectPtr = static_cast<UObject*>(Object))
    {
        // UObject 路径：按类反射查找属性。
        if (IsValid(ObjectPtr))
        {
            Property = ObjectPtr->GetClass()->FindPropertyByName(*PropertyName);
        }
    }
    else
    {
        // 结构体实例路径（UScriptStruct 内存）：从测试等场景传入，无法按名反射查找，
        // 结构体属性的属性名由调用方持有 UScriptStruct 后解析，此处不支持。
        OutErrorMessage = TEXT("Unsupported object payload");
        return false;
    }
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    const bool bWrongBool = Property->IsA<FBoolProperty>() && Value->Type != EJson::Boolean;
    const bool bWrongString = Property->IsA<FStrProperty>() && Value->Type != EJson::String;
    const bool bNumericProperty = Property->IsA<FIntProperty>() || Property->IsA<FFloatProperty>();
    const bool bWrongNumber = bNumericProperty && Value->Type != EJson::Number;
    if (bWrongBool || bWrongString || bWrongNumber)
    {
        OutErrorMessage = FString::Printf(TEXT("JSON type does not match property %s"), *PropertyName);
        return false;
    }
    if (bNumericProperty)
    {
        const double Number = Value->AsNumber();
        if (!FMath::IsFinite(Number) ||
            (Property->IsA<FFloatProperty>() && FMath::Abs(Number) > MAX_flt) ||
            (Property->IsA<FIntProperty>() && (Number < MIN_int32 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number)))
        {
            OutErrorMessage = FString::Printf(TEXT("Value is outside the numeric type of property %s"), *PropertyName);
            return false;
        }
    }
    // FClassProperty 继承 FObjectProperty，必须先处理类类型，避免把 UClass* 路径误判成普通对象路径。
    if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
    {
        if (Value->Type != EJson::String)
        {
            OutErrorMessage = FString::Printf(TEXT("Expected a string class path for property %s"), *PropertyName);
            return false;
        }
        const FString ClassPath = Value->AsString();
        if (ClassPath.IsEmpty())
        {
            ClassProp->SetObjectPropertyValue_InContainer(Object, nullptr);
            return true;
        }
        UClass* ClassValue = LoadClass<UObject>(nullptr, *ClassPath);
        UClass* MetaClass = ClassProp->MetaClass ? ClassProp->MetaClass : UObject::StaticClass();
        if (!ClassValue || !ClassValue->IsChildOf(MetaClass))
        {
            OutErrorMessage = FString::Printf(TEXT("'%s' is not a valid %s for property %s"),
                *ClassPath, *MetaClass->GetName(), *PropertyName);
            return false;
        }
        ClassProp->SetObjectPropertyValue_InContainer(Object, ClassValue);
        return true;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);

    // Handle different property types
    if (Property->IsA<FBoolProperty>())
    {
        ((FBoolProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsBool());
        return true;
    }
    else if (Property->IsA<FIntProperty>())
    {
        int32 IntValue = static_cast<int32>(Value->AsNumber());
        FIntProperty* IntProperty = CastField<FIntProperty>(Property);
        if (IntProperty)
        {
            IntProperty->SetPropertyValue_InContainer(Object, IntValue);
            return true;
        }
    }
    else if (Property->IsA<FFloatProperty>())
    {
        ((FFloatProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsNumber());
        return true;
    }
    else if (Property->IsA<FStrProperty>())
    {
        ((FStrProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsString());
        return true;
    }
    // 材质表达式的 ParameterName/Group 是 FName；缺少这一支就只能改注释文字这类字符串属性。
    else if (Property->IsA<FNameProperty>())
    {
        if (Value->Type != EJson::String)
        {
            OutErrorMessage = FString::Printf(TEXT("Expected a string for name property %s"), *PropertyName);
            return false;
        }
        const FString NameValue = Value->AsString();
        if (NameValue.Len() >= NAME_SIZE || (!NameValue.IsEmpty() && !FName::IsValidXName(NameValue, INVALID_OBJECTNAME_CHARACTERS)))
        {
            OutErrorMessage = FString::Printf(TEXT("Invalid name value for property %s"), *PropertyName);
            return false;
        }
        ((FNameProperty*)Property)->SetPropertyValue(PropertyAddr, FName(*NameValue));
        return true;
    }
    // 表达式引用属性（如 TextureSample.Texture）用资产路径赋值；空串表示清空。
    else if (Property->IsA<FObjectProperty>())
    {
        FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property);
        if (!ObjectProp)
        {
            OutErrorMessage = FString::Printf(TEXT("Unsupported object property %s"), *PropertyName);
            return false;
        }
        if (Value->Type != EJson::String)
        {
            OutErrorMessage = FString::Printf(TEXT("Expected an object path string for property %s"), *PropertyName);
            return false;
        }
        const FString ObjectPath = Value->AsString();
        if (ObjectPath.IsEmpty())
        {
            ObjectProp->SetObjectPropertyValue(PropertyAddr, nullptr);
            return true;
        }
        UObject* LoadedValue = LoadObject<UObject>(nullptr, *ObjectPath);
        if (!LoadedValue)
        {
            OutErrorMessage = FString::Printf(TEXT("Failed to load object '%s' for property %s"), *ObjectPath, *PropertyName);
            return false;
        }
        if (ObjectProp->PropertyClass && !LoadedValue->IsA(ObjectProp->PropertyClass))
        {
            OutErrorMessage = FString::Printf(TEXT("'%s' is not a %s (property %s)"),
                *ObjectPath, *ObjectProp->PropertyClass->GetName(), *PropertyName);
            return false;
        }
        ObjectProp->SetObjectPropertyValue(PropertyAddr, LoadedValue);
        return true;
    }
    else if (Property->IsA<FByteProperty>() || Property->IsA<FEnumProperty>())
    {
        FByteProperty* ByteProp = CastField<FByteProperty>(Property);
        FEnumProperty* EnumProp = CastField<FEnumProperty>(Property);
        UEnum* EnumDef = ByteProp ? ByteProp->GetIntPropertyEnum() : EnumProp->GetEnum();
        double Number = 0.0;
        bool bNumeric = Value->Type == EJson::Number;
        int64 EnumValue = INDEX_NONE;
        if (bNumeric)
        {
            Number = Value->AsNumber();
        }
        else if (Value->Type == EJson::String && EnumDef)
        {
            const FString Text = Value->AsString();
            if (Text.Len() >= NAME_SIZE)
            {
                OutErrorMessage = TEXT("Enum name is too long");
                return false;
            }
            bNumeric = Text.IsNumeric();
            if (bNumeric)
            {
                bool bHasDigit = false;
                for (TCHAR Character : Text)
                    bHasDigit |= Character >= TEXT('0') && Character <= TEXT('9');
                if (!bHasDigit)
                {
                    OutErrorMessage = TEXT("Numeric enum strings must contain digits");
                    return false;
                }
                Number = FCString::Atod(*Text);
            }
            else
            {
                if (EnumDef->GenerateFullEnumName(*Text).Len() >= NAME_SIZE)
                {
                    OutErrorMessage = TEXT("Qualified enum name is too long");
                    return false;
                }
                EnumValue = EnumDef->GetValueByNameString(Text);
            }
        }
        else
        {
            OutErrorMessage = TEXT("Expected a numeric byte value or an enum name/value");
            return false;
        }
        if (bNumeric)
        {
            if (!FMath::IsFinite(Number) || FMath::FloorToDouble(Number) != Number ||
                Number < MIN_int32 || Number > MAX_int32 || (ByteProp && (Number < 0 || Number > 255)))
            {
                OutErrorMessage = TEXT("Enum or byte value is outside its numeric range");
                return false;
            }
            EnumValue = static_cast<int64>(Number);
        }
        if (EnumDef)
        {
            const FString EnumName = EnumDef->GetNameStringByValue(EnumValue);
            if (!EnumDef->IsValidEnumValue(EnumValue) || EnumName.EndsWith(TEXT("_MAX")) || EnumName.EndsWith(TEXT("_NUM")))
            {
                OutErrorMessage = TEXT("Value does not identify a valid enum member");
                return false;
            }
        }
        if (ByteProp) ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
        else EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(PropertyAddr, EnumValue);
        return true;
    }
    else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
    {
        // 支持常用结构体属性的 JSON 数组或字符串赋值；未支持的结构体仍报错。
        const UScriptStruct* StructDef = StructProp->Struct;
        if (!StructDef)
        {
            OutErrorMessage = TEXT("Struct property has no struct definition");
            return false;
        }
        const FString StructName = StructDef->GetName();

        TArray<double> Numbers;
        if (Value->Type == EJson::Array)
        {
            // 4.26 的 AsArray() 返回 const TArray&，不能解引用指针。
            const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
            for (const TSharedPtr<FJsonValue>& Element : Array)
            {
                if (!Element.IsValid() || Element->Type != EJson::Number)
                {
                    OutErrorMessage = TEXT("Struct array must contain only numbers");
                    return false;
                }
                Numbers.Add(Element->AsNumber());
            }
        }
        else if (Value->Type == EJson::String)
        {
            // 优先使用 UE 原生解析器，兼容 X=1 Y=2 等格式。
            const FString Text = Value->AsString();
            if (StructDef == TBaseStructure<FVector2D>::Get())
            {
                FVector2D Parsed;
                if (Parsed.InitFromString(Text))
                {
                    *static_cast<FVector2D*>(PropertyAddr) = Parsed;
                    return true;
                }
            }
            if (StructDef == TBaseStructure<FVector>::Get())
            {
                FVector Parsed;
                if (Parsed.InitFromString(Text))
                {
                    *static_cast<FVector*>(PropertyAddr) = Parsed;
                    return true;
                }
            }
            if (StructDef == TBaseStructure<FRotator>::Get())
            {
                FRotator Parsed;
                if (Parsed.InitFromString(Text))
                {
                    *static_cast<FRotator*>(PropertyAddr) = Parsed;
                    return true;
                }
            }
            // FIntPoint 无 TBaseStructure 特化，按结构体名匹配；用引擎 ToString 格式 X=.. Y=..
            if (StructName == TEXT("IntPoint"))
            {
                FIntPoint Parsed(0, 0);
                if (FParse::Value(*Text, TEXT("X="), Parsed.X) && FParse::Value(*Text, TEXT("Y="), Parsed.Y))
                {
                    *static_cast<FIntPoint*>(PropertyAddr) = Parsed;
                    UE_LOG(LogTemp, Display, TEXT("Setting IntPoint property %s to (%d, %d)"),
                        *PropertyName, Parsed.X, Parsed.Y);
                    return true;
                }
            }
            // 兼容纯数字逗号列表，但非法数值不能静默变成零。
            TArray<FString> Parts;
            Text.ParseIntoArray(Parts, TEXT(","), true);
            for (const FString& Part : Parts)
            {
                FString NumPart = Part;
                if (Part.Contains(TEXT("=")))
                {
                    FString Key;
                    Part.Split(TEXT("="), &Key, &NumPart);
                }
                NumPart.TrimStartInline();
                NumPart.TrimEndInline();
                if (!NumPart.IsNumeric())
                {
                    OutErrorMessage = TEXT("Invalid struct numeric value");
                    return false;
                }
                Numbers.Add(FCString::Atod(*NumPart));
            }
        }

        if (StructDef == TBaseStructure<FLinearColor>::Get())
        {
            // 材质参数默认值与常量节点都是 FLinearColor；3 分量补 alpha=1，避免沿用旧值。
            if (Numbers.Num() < 3 || Numbers.Num() > 4)
            {
                OutErrorMessage = FString::Printf(TEXT("LinearColor property %s needs 3 or 4 numbers"), *PropertyName);
                return false;
            }
            for (const double Channel : Numbers)
            {
                if (!FMath::IsFinite(Channel) || FMath::Abs(Channel) > MAX_flt)
                {
                    OutErrorMessage = FString::Printf(TEXT("LinearColor property %s needs finite numbers"), *PropertyName);
                    return false;
                }
            }
            *static_cast<FLinearColor*>(PropertyAddr) = FLinearColor(
                (float)Numbers[0], (float)Numbers[1], (float)Numbers[2],
                Numbers.Num() >= 4 ? (float)Numbers[3] : 1.0f);
            return true;
        }
        if (StructDef == TBaseStructure<FVector2D>::Get())
        {
            if (Numbers.Num() < 2)
            {
                OutErrorMessage = FString::Printf(TEXT("Vector2D property %s needs 2 numbers"), *PropertyName);
                return false;
            }
            *static_cast<FVector2D*>(PropertyAddr) = FVector2D(Numbers[0], Numbers[1]);
            UE_LOG(LogTemp, Display, TEXT("Setting Vector2D property %s to (%f, %f)"), *PropertyName, Numbers[0], Numbers[1]);
            return true;
        }
        if (StructDef == TBaseStructure<FVector>::Get())
        {
            if (Numbers.Num() < 3)
            {
                OutErrorMessage = FString::Printf(TEXT("Vector property %s needs 3 numbers"), *PropertyName);
                return false;
            }
            *static_cast<FVector*>(PropertyAddr) = FVector(Numbers[0], Numbers[1], Numbers[2]);
            UE_LOG(LogTemp, Display, TEXT("Setting Vector property %s to (%f, %f, %f)"), *PropertyName, Numbers[0], Numbers[1], Numbers[2]);
            return true;
        }
        if (StructDef == TBaseStructure<FRotator>::Get())
        {
            if (Numbers.Num() < 3)
            {
                OutErrorMessage = FString::Printf(TEXT("Rotator property %s needs 3 numbers"), *PropertyName);
                return false;
            }
            *static_cast<FRotator*>(PropertyAddr) = FRotator(Numbers[0], Numbers[1], Numbers[2]);
            UE_LOG(LogTemp, Display, TEXT("Setting Rotator property %s to (P%f, Y%f, R%f)"), *PropertyName, Numbers[0], Numbers[1], Numbers[2]);
            return true;
        }
        // FIntPoint：数组形态（[1800, 460]）。字符串形态已在上方 X=/Y= 分支处理。
        if (StructName == TEXT("IntPoint"))
        {
            if (Numbers.Num() < 2)
            {
                OutErrorMessage = FString::Printf(TEXT("IntPoint property %s needs 2 numbers"), *PropertyName);
                return false;
            }
            *static_cast<FIntPoint*>(PropertyAddr) = FIntPoint(
                static_cast<int32>(Numbers[0]), static_cast<int32>(Numbers[1]));
            UE_LOG(LogTemp, Display, TEXT("Setting IntPoint property %s to (%d, %d)"),
                *PropertyName, static_cast<int32>(Numbers[0]), static_cast<int32>(Numbers[1]));
            return true;
        }

        OutErrorMessage = FString::Printf(TEXT("Unsupported struct type: %s for property %s"), *StructName, *PropertyName);
        return false;
    }
    else if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
    {
        // TSet 属性：接受 JSON 数组或逗号分隔字符串（"a, b, c"）；元素支持字符串（FName）或数字（枚举整值）。
        TSharedPtr<FJsonValue> ArrayValue = Value;
        TArray<TSharedPtr<FJsonValue>> StringElements;
        if (Value->Type == EJson::String)
        {
            TArray<FString> Parts;
            Value->AsString().ParseIntoArray(Parts, TEXT(","), true);
            for (const FString& Part : Parts)
            {
                FString Trimmed = Part;
                Trimmed.TrimStartInline();
                Trimmed.TrimEndInline();
                StringElements.Add(MakeShared<FJsonValueString>(Trimmed));
            }
            ArrayValue = MakeShared<FJsonValueArray>(StringElements);
        }
        if (ArrayValue->Type != EJson::Array)
        {
            OutErrorMessage = FString::Printf(TEXT("Set property %s needs a JSON array or comma-separated string"), *PropertyName);
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Elements = ArrayValue->AsArray();
        FProperty* ElementProp = UnrealMCPSetHelper::GetElementProperty(SetProp);
        // 先整体校验元素类型与取值，全部合法后才清空写入，失败时不破坏原集合。
        TArray<FName> NameEntries;
        TArray<int32> IntEntries;
        TArray<FString> StrEntries;
        for (const TSharedPtr<FJsonValue>& Element : Elements)
        {
            if (!Element.IsValid())
            {
                OutErrorMessage = TEXT("Set array contains invalid element");
                return false;
            }
            if (CastField<FNameProperty>(ElementProp))
            {
                if (Element->Type != EJson::String)
                {
                    OutErrorMessage = TEXT("FName set elements must be strings");
                    return false;
                }
                NameEntries.Add(FName(*Element->AsString()));
            }
            else if (CastField<FIntProperty>(ElementProp))
            {
                if (Element->Type != EJson::Number)
                {
                    OutErrorMessage = TEXT("Integer set elements must be numbers");
                    return false;
                }
                IntEntries.Add(static_cast<int32>(Element->AsNumber()));
            }
            else if (CastField<FStrProperty>(ElementProp))
            {
                if (Element->Type != EJson::String)
                {
                    OutErrorMessage = TEXT("String set elements must be strings");
                    return false;
                }
                StrEntries.Add(Element->AsString());
            }
            else
            {
                OutErrorMessage = FString::Printf(TEXT("Unsupported set element type: %s for property %s"),
                    *ElementProp->GetClass()->GetName(), *PropertyName);
                return false;
            }
        }
        FScriptSetHelper SetHelper(SetProp, PropertyAddr);
        SetHelper.EmptyElements();
        // 去重由集合语义保证，逐条 AddElement 即可。
        for (const FName& Entry : NameEntries)
        {
            SetHelper.AddElement(&Entry);
        }
        for (const int32& Entry : IntEntries)
        {
            SetHelper.AddElement(&Entry);
        }
        for (const FString& Entry : StrEntries)
        {
            SetHelper.AddElement(&Entry);
        }
        SetHelper.Rehash();
        UE_LOG(LogTemp, Display, TEXT("Setting set property %s with %d elements"), *PropertyName, Elements.Num());
        return true;
    }

    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"),
                                    *Property->GetClass()->GetName(), *PropertyName);
    return false;
}
bool FUnrealMCPCommonUtils::SaveLoadedAssetReliable(UObject* Asset, FString& OutError)
{
    // 首选 UEditorAssetLibrary；AssetRegistry 未就绪等内部状态会使其拒绝保存。
    if (UEditorAssetLibrary::SaveLoadedAsset(Asset, false))
    {
        return true;
    }
    if (!Asset)
    {
        OutError = TEXT("Invalid asset");
        return false;
    }
    // 回退：与 add_row 相同的 UPackage 直存路径，不依赖 AssetRegistry。
    UPackage* AssetPackage = Asset->GetOutermost();
    if (!AssetPackage)
    {
        OutError = TEXT("Asset has no package");
        return false;
    }
    const FString AssetFilename = FPackageName::LongPackageNameToFilename(
        AssetPackage->GetName(), FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(AssetPackage, Asset, RF_Public | RF_Standalone,
        *AssetFilename, GError, nullptr, false, true, SAVE_NoError))
    {
        OutError = FString::Printf(TEXT("SavePackage failed: %s"), *AssetFilename);
        return false;
    }
    UE_LOG(LogTemp, Log, TEXT("SaveLoadedAssetReliable - fallback SavePackage succeeded: %s"), *AssetFilename);
    return true;
}
bool FUnrealMCPCommonUtils::ShouldDeferSave(const TSharedPtr<FJsonObject>& Params)
{
    // 批次语义：客户端传 defer_save=true 则跳过落盘，由 save_assets 统一提交；
    // 未保存的资产在编辑器里仍可 Ctrl+Z 回退，这是轻量版批次能接受的回滚方式。
    bool bDefer = false;
    if (Params)
    {
        Params->TryGetBoolField(TEXT("defer_save"), bDefer);
    }
    return bDefer;
}

bool FUnrealMCPCommonUtils::SaveUnlessDeferred(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
    if (!Asset)
    {
        OutError = TEXT("Invalid asset");
        return false;
    }
    return ShouldDeferSave(Params) || SaveLoadedAssetReliable(Asset, OutError);
}

bool FUnrealMCPCommonUtils::CompileBlueprintWithDiagnostics(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutDiagnostics, FString& OutError)
{
    OutDiagnostics = MakeShared<FJsonObject>();
    if (!Blueprint)
    {
        OutError = TEXT("Blueprint unavailable");
        return false;
    }

    // FCompilerResultsLog 收集逐条 error/warning；KismetCompiler 已是模块依赖。
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Warnings;
    FString FirstError;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
        {
            continue;
        }
        const FString Text = Message->ToText().ToString();
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("message"), Text);
        if (Severity == EMessageSeverity::Error)
        {
            if (FirstError.IsEmpty())
            {
                FirstError = Text;
            }
            Errors.Add(MakeShared<FJsonValueObject>(Item));
        }
        else
        {
            Warnings.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    OutDiagnostics->SetNumberField(TEXT("num_errors"), Results.NumErrors);
    OutDiagnostics->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
    OutDiagnostics->SetArrayField(TEXT("errors"), Errors);
    OutDiagnostics->SetArrayField(TEXT("warnings"), Warnings);

    const bool bOk = Blueprint->Status != BS_Error && Results.NumErrors == 0;
    if (!bOk)
    {
        // 汇总串带上第一条错误，客户端不必再解析数组就能知道大概原因。
        OutError = FirstError.IsEmpty()
            ? FString::Printf(TEXT("Blueprint compilation failed (%d errors)"), Results.NumErrors)
            : FString::Printf(TEXT("Blueprint compilation failed (%d errors): %s"), Results.NumErrors, *FirstError);
    }
    return bOk;
}
