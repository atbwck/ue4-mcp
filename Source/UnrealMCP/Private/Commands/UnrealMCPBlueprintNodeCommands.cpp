#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/InputSettings.h"
#include "Camera/CameraActor.h"
#include "Kismet/GameplayStatics.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "MCPGraphLayout.h"

// Declare the log category
DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCP, Log, All);

FUnrealMCPBlueprintNodeCommands::FUnrealMCPBlueprintNodeCommands()
{
}

namespace
{
// 只读命令不进事务，其余命令整体一个事务，让编辑器能一次撤销整条命令的全部改动。
bool IsReadOnlyNodeCommand(const FString& CommandType)
{
    return CommandType == TEXT("get_blueprint_graph")
        || CommandType == TEXT("find_blueprint_nodes")
        || CommandType == TEXT("get_blueprint_node_details")
        || CommandType == TEXT("list_blueprint_graphs");
}
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (IsReadOnlyNodeCommand(CommandType))
    {
        return DispatchCommand(CommandType, Params);
    }
    const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "BlueprintNodeCommand", "UnrealMCP Blueprint Node Command"));
    // Capture objects before mutation; a transaction alone records no undo data.
    FString Name;
    if (Params->TryGetStringField(TEXT("blueprint_name"), Name))
    {
        if (UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(Name))
        {
            Blueprint->Modify();
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            for (UEdGraph* Graph : Graphs)
            {
                Graph->Modify();
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node) { Node->SetFlags(RF_Transactional); Node->Modify(); }
                }
            }
        }
    }
    TSharedPtr<FJsonObject> Result = DispatchCommand(CommandType, Params);
    Result->SetBoolField(TEXT("deferred_save"), FUnrealMCPCommonUtils::ShouldDeferSave(Params));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::DispatchCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("list_blueprint_graphs") || CommandType == TEXT("create_blueprint_graph"))
    {
        return HandleBlueprintGraphs(CommandType, Params);
    }
    if (CommandType == TEXT("format_blueprint_graph") || CommandType == TEXT("get_blueprint_graph"))
    {
        return HandleGraphLayout(CommandType, Params);
    }
    if (CommandType == TEXT("connect_blueprint_nodes"))
    {
        return HandleConnectBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("add_blueprint_get_self_component_reference"))
    {
        return HandleAddBlueprintGetSelfComponentReference(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_node"))
    {
        return HandleAddBlueprintEvent(Params);
    }
    else if (CommandType == TEXT("add_blueprint_function_node"))
    {
        return HandleAddBlueprintFunctionCall(Params);
    }
    else if (CommandType == TEXT("add_blueprint_variable"))
    {
        return HandleAddBlueprintVariable(Params);
    }
    else if (CommandType == TEXT("add_blueprint_input_action_node"))
    {
        return HandleAddBlueprintInputActionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_self_reference"))
    {
        return HandleAddBlueprintSelfReference(Params);
    }
    else if (CommandType == TEXT("find_blueprint_nodes"))
    {
        return HandleFindBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("add_blueprint_cast_node"))
    {
        return HandleAddBlueprintCastNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_external_member_get_node"))
    {
        return HandleAddBlueprintExternalMemberGetNode(Params);
    }
    else if (CommandType == TEXT("get_blueprint_node_details"))
    {
        return HandleGetBlueprintNodeDetails(Params);
    }
    else if (CommandType == TEXT("delete_blueprint_node"))
    {
        return HandleDeleteBlueprintNode(Params);
    }
    else if (CommandType == TEXT("disconnect_blueprint_nodes"))
    {
        return HandleDisconnectBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("prune_unused_nodes"))
    {
        return HandlePruneUnusedNodes(Params);
    }
    else if (CommandType == TEXT("set_blueprint_pin_default"))
    {
        return HandleSetBlueprintPinDefault(Params);
    }
    else if (CommandType == TEXT("set_blueprint_node_property"))
    {
        return HandleSetBlueprintNodeProperty(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint node command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleBlueprintGraphs(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing blueprint_name"));
    }
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(Name);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found"));
    }
    if (CommandType == TEXT("list_blueprint_graphs"))
    {
        TArray<TSharedPtr<FJsonValue>> Graphs;
        auto Append = [&Graphs](const TArray<UEdGraph*>& Pages, const FString& Kind)
        {
            for (UEdGraph* Graph : Pages)
            {
                if (!Graph) { continue; }
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("graph_name"), Graph->GetName());
                Item->SetStringField(TEXT("graph_type"), Kind);
                Item->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
                Graphs.Add(MakeShared<FJsonValueObject>(Item));
            }
        };
        Append(Blueprint->UbergraphPages, TEXT("event"));
        Append(Blueprint->FunctionGraphs, TEXT("function"));
        Append(Blueprint->MacroGraphs, TEXT("macro"));
        auto Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("graphs"), Graphs);
        return Result;
    }
    FString GraphName, Kind;
    if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty() ||
        GraphName.Len() >= NAME_SIZE || GraphName.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
        !FName::IsValidXName(GraphName, INVALID_OBJECTNAME_CHARACTERS))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid graph_name"));
    }
    if (!Params->TryGetStringField(TEXT("graph_type"), Kind) ||
        (Kind != TEXT("function") && Kind != TEXT("macro")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("graph_type must be function or macro"));
    }
    TArray<UEdGraph*> Existing;
    Blueprint->GetAllGraphs(Existing);
    for (UEdGraph* Graph : Existing)
    {
        if (Graph && Graph->GetFName() == FName(*GraphName))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Graph name already exists"));
        }
    }
    FKismetNameValidator NameValidator(Blueprint, NAME_None, nullptr);
    if (Blueprint->BlueprintType != BPTYPE_Normal ||
        NameValidator.IsValid(GraphName) != EValidatorResult::Ok ||
        (Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindFunctionByName(*GraphName)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Requires a normal Blueprint and a name that does not shadow an existing function"));
    }
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*GraphName),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    Graph->SetFlags(RF_Transactional);
    if (Kind == TEXT("function"))
    {
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
    }
    else
    {
        FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FString Error;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graph_name"), Graph->GetName());
    Result->SetStringField(TEXT("graph_type"), Kind);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleGraphLayout(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), Name))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing blueprint_name"));
    }
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(Name);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint not found"));
    }
    // 排版与图 introspection 共用图作用域：缺省为第一个 Event Graph，可指定函数图/宏图。
    // 读命令（get_blueprint_graph）只查不建，否则一次查询会改到蓝图。
    FString GraphError;
    const bool bReadOnly = CommandType == TEXT("get_blueprint_graph");
    UEdGraph* Graph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError, !bReadOnly);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Blueprint has no Event Graph") : GraphError);
    }
    if (CommandType == TEXT("get_blueprint_graph"))
    {
        return MCPGraphLayout::Describe(Graph);
    }
    TSharedPtr<FJsonObject> Result = MCPGraphLayout::Format(Graph, Params);
    if (Result->HasField(TEXT("error")))
    {
        return Result;
    }
    // 干跑只返回计划坐标，不得标脏或落盘
    bool bDryRun = false;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    if (bDryRun)
    {
        return Result;
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph layout changed but save failed: %s"), *SaveError));
    }
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleConnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString SourceNodeId;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_node_id' parameter"));
    }

    FString TargetNodeId;
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_node_id' parameter"));
    }

    FString SourcePinName;
    if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_pin' parameter"));
    }

    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_pin' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // Find the nodes
    UEdGraphNode* SourceNode = nullptr;
    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node->NodeGuid.ToString() == SourceNodeId)
        {
            SourceNode = Node;
        }
        else if (Node->NodeGuid.ToString() == TargetNodeId)
        {
            TargetNode = Node;
        }
    }

    if (!SourceNode || !TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source or target node not found"));
    }

    // Connect the nodes
    if (FUnrealMCPCommonUtils::ConnectGraphNodes(EventGraph, SourceNode, SourcePinName, TargetNode, TargetPinName))
    {
        // Mark the blueprint as modified
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        FString SaveError;
        if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("source_node_id"), SourceNodeId);
        ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
        return ResultObj;
    }

    // 失败时列出两侧可用引脚，便于调用方修正 pin 名。
    FString SourcePins, TargetPins;
    for (UEdGraphPin* Pin : SourceNode->Pins)
    {
        SourcePins += (SourcePins.IsEmpty() ? TEXT("") : TEXT(", ")) + Pin->PinName.ToString() + (Pin->Direction == EGPD_Output ? TEXT("(out)") : TEXT("(in)"));
    }
    for (UEdGraphPin* Pin : TargetNode->Pins)
    {
        TargetPins += (TargetPins.IsEmpty() ? TEXT("") : TEXT(", ")) + Pin->PinName.ToString() + (Pin->Direction == EGPD_Output ? TEXT("(out)") : TEXT("(in)"));
    }
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to connect nodes. Source pins: [%s]. Target pins: [%s]"), *SourcePins, *TargetPins));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintGetSelfComponentReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // We'll skip component verification since the GetAllNodes API may have changed in UE5.5

    // Create the variable get node directly
    UK2Node_VariableGet* GetComponentNode = NewObject<UK2Node_VariableGet>(EventGraph);
    if (!GetComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create get component node"));
    }

    // Set up the variable reference properly for UE5.5
    FMemberReference& VarRef = GetComponentNode->VariableReference;
    VarRef.SetSelfMember(FName(*ComponentName));

    // Set node position
    GetComponentNode->NodePosX = NodePosition.X;
    GetComponentNode->NodePosY = NodePosition.Y;

    // Add to graph
    EventGraph->AddNode(GetComponentNode);
    GetComponentNode->CreateNewGuid();
    GetComponentNode->PostPlacedNewNode();
    GetComponentNode->AllocateDefaultPins();

    // Explicitly reconstruct node for UE5.5
    GetComponentNode->ReconstructNode();

    // Mark the blueprint as modified
    MCPGraphLayout::PlaceNewNode(GetComponentNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), GetComponentNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEvent(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    if (!Blueprint->UbergraphPages.Contains(EventGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event nodes are only valid in Event Graphs"));
    }

    // Create the event node
    const bool bExisting = FUnrealMCPCommonUtils::FindExistingEventNode(EventGraph, EventName) != nullptr;
    UK2Node_Event* EventNode = FUnrealMCPCommonUtils::CreateEventNode(EventGraph, EventName, NodePosition);
    if (!EventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create event node"));
    }

    // Mark the blueprint as modified
    if (!bExisting)
    {
        MCPGraphLayout::PlaceNewNode(EventNode, Params);
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
    ResultObj->SetBoolField(TEXT("already_exists"), bExisting);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'function_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Check for target parameter (optional)
    FString Target;
    Params->TryGetStringField(TEXT("target"), Target);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // Find the function
    UFunction* Function = nullptr;
    UK2Node_CallFunction* FunctionNode = nullptr;

    // Add extensive logging for debugging
    UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in target '%s'"),
           *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target);

    // Check if we have a target class specified
    if (!Target.IsEmpty())
    {
        // Try to find the target class
        UClass* TargetClass = nullptr;

        // First try without a prefix
        TargetClass = FindObject<UClass>(ANY_PACKAGE, *Target);
        UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"),
               *Target, TargetClass ? TEXT("Found") : TEXT("Not found"));

        // If not found, try with U prefix (common convention for UE classes)
        if (!TargetClass && !Target.StartsWith(TEXT("U")))
        {
            FString TargetWithPrefix = FString(TEXT("U")) + Target;
            TargetClass = FindObject<UClass>(ANY_PACKAGE, *TargetWithPrefix);
            UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"),
                   *TargetWithPrefix, TargetClass ? TEXT("Found") : TEXT("Not found"));
        }

        // If still not found, try with common component names
        if (!TargetClass)
        {
            // Try some common component class names
            TArray<FString> PossibleClassNames;
            PossibleClassNames.Add(FString(TEXT("U")) + Target + TEXT("Component"));
            PossibleClassNames.Add(Target + TEXT("Component"));

            for (const FString& ClassName : PossibleClassNames)
            {
                TargetClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
                if (TargetClass)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found class using alternative name '%s'"), *ClassName);
                    break;
                }
            }
        }

        // Special case handling for common classes like UGameplayStatics
        if (!TargetClass && Target == TEXT("UGameplayStatics"))
        {
            // For UGameplayStatics, use a direct reference to known class
            TargetClass = FindObject<UClass>(ANY_PACKAGE, TEXT("UGameplayStatics"));
            if (!TargetClass)
            {
                // Try loading it from its known package
                TargetClass = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"));
                UE_LOG(LogTemp, Display, TEXT("Explicitly loading GameplayStatics: %s"),
                       TargetClass ? TEXT("Success") : TEXT("Failed"));
            }
        }

        // If we found a target class, look for the function there
        if (TargetClass)
        {
            UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in class '%s'"),
                   *FunctionName, *TargetClass->GetName());

            // First try exact name
            Function = TargetClass->FindFunctionByName(*FunctionName);

            // If not found, try class hierarchy
            UClass* CurrentClass = TargetClass;
            while (!Function && CurrentClass)
            {
                UE_LOG(LogTemp, Display, TEXT("Searching in class: %s"), *CurrentClass->GetName());

                // Try exact match
                Function = CurrentClass->FindFunctionByName(*FunctionName);

                // Try case-insensitive match
                if (!Function)
                {
                    for (TFieldIterator<UFunction> FuncIt(CurrentClass); FuncIt; ++FuncIt)
                    {
                        UFunction* AvailableFunc = *FuncIt;
                        UE_LOG(LogTemp, Display, TEXT("  - Available function: %s"), *AvailableFunc->GetName());

                        if (AvailableFunc->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive match: %s"), *AvailableFunc->GetName());
                            Function = AvailableFunc;
                            break;
                        }
                    }
                }

                // Move to parent class
                CurrentClass = CurrentClass->GetSuperClass();
            }

            // Special handling for known functions
            if (!Function)
            {
                if (TargetClass->GetName() == TEXT("GameplayStatics") &&
                    (FunctionName == TEXT("GetActorOfClass") || FunctionName.Equals(TEXT("GetActorOfClass"), ESearchCase::IgnoreCase)))
                {
                    UE_LOG(LogTemp, Display, TEXT("Using special case handling for GameplayStatics::GetActorOfClass"));

                    // Create the function node directly
                    FunctionNode = NewObject<UK2Node_CallFunction>(EventGraph);
                    if (FunctionNode)
                    {
                        // Direct setup for known function
                        FunctionNode->FunctionReference.SetExternalMember(
                            FName(TEXT("GetActorOfClass")),
                            TargetClass
                        );

                        FunctionNode->NodePosX = NodePosition.X;
                        FunctionNode->NodePosY = NodePosition.Y;
                        EventGraph->AddNode(FunctionNode);
                        FunctionNode->CreateNewGuid();
                        FunctionNode->PostPlacedNewNode();
                        FunctionNode->AllocateDefaultPins();

                        UE_LOG(LogTemp, Display, TEXT("Created GetActorOfClass node directly"));

                        // List all pins
                        for (UEdGraphPin* Pin : FunctionNode->Pins)
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Pin: %s, Direction: %d, Category: %s"),
                                   *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
                        }
                    }
                }
            }
        }
    }

    // If we still haven't found the function, try in the blueprint's class
    if (!Function && !FunctionNode)
    {
        UE_LOG(LogTemp, Display, TEXT("Trying to find function in blueprint class"));
        if (Blueprint->GeneratedClass)
        {
            Function = Blueprint->GeneratedClass->FindFunctionByName(*FunctionName);
        }
    }

    if (Function && !Blueprint->UbergraphPages.Contains(EventGraph) &&
        Function->HasMetaData(TEXT("Latent")) && Blueprint->FunctionGraphs.Contains(EventGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Latent functions cannot be placed in function graphs"));
    }
    // Create the function call node if we found the function
    if (Function && !FunctionNode)
    {
        FunctionNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(EventGraph, Function, NodePosition);
    }

    if (!FunctionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function not found: %s in target %s. For newly added C++ UFUNCTIONs, rebuild the Editor target and restart the editor, then refresh_project_reflection. Graph refresh alone cannot load new native reflection."), *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target));
    }

    // Set parameters if provided
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* ParamsObj;
        if (Params->TryGetObjectField(TEXT("params"), ParamsObj))
        {
            // Process parameters
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamsObj)->Values)
            {
                const FString& ParamName = Param.Key;
                const TSharedPtr<FJsonValue>& ParamValue = Param.Value;

                // Find the parameter pin
                UEdGraphPin* ParamPin = FUnrealMCPCommonUtils::FindPin(FunctionNode, ParamName, EGPD_Input);
                if (ParamPin)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found parameter pin '%s' of category '%s'"),
                           *ParamName, *ParamPin->PinType.PinCategory.ToString());
                    UE_LOG(LogTemp, Display, TEXT("  Current default value: '%s'"), *ParamPin->DefaultValue);
                    if (ParamPin->PinType.PinSubCategoryObject.IsValid())
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Pin subcategory: '%s'"),
                               *ParamPin->PinType.PinSubCategoryObject->GetName());
                    }

                    // Set parameter based on type
                    if (ParamValue->Type == EJson::String)
                    {
                        FString StringVal = ParamValue->AsString();
                        UE_LOG(LogTemp, Display, TEXT("  Setting string parameter '%s' to: '%s'"),
                               *ParamName, *StringVal);

                        // Handle class reference parameters (e.g., ActorClass in GetActorOfClass)
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
                        {
                            // For class references, we require the exact class name with proper prefix
                            // - Actor classes must start with 'A' (e.g., ACameraActor)
                            // - Non-actor classes must start with 'U' (e.g., UObject)
                            const FString& ClassName = StringVal;

                            // TODO: This likely won't work in UE5.5+, so don't rely on it.
                            UClass* Class = FindObject<UClass>(ANY_PACKAGE, *ClassName);

                            if (!Class)
                            {
                                Class = LoadObject<UClass>(nullptr, *ClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("FindObject<UClass> failed. Assuming soft path  path: %s"), *ClassName);
                            }

                            // If not found, try with Engine module path
                            if (!Class)
                            {
                                FString EngineClassName = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
                                Class = LoadObject<UClass>(nullptr, *EngineClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("Trying Engine module path: %s"), *EngineClassName);
                            }

                            if (!Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to find class '%s'. Make sure to use the exact class name with proper prefix (A for actors, U for non-actors)"), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to find class '%s'"), *ClassName));
                            }

                            const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(EventGraph->GetSchema());
                            if (!K2Schema)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to get K2Schema"));
                                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2Schema"));
                            }

                            K2Schema->TrySetDefaultObject(*ParamPin, Class);
                            if (ParamPin->DefaultObject != Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set class reference for pin '%s'"), *ParamPin->PinName.ToString()));
                            }

                            UE_LOG(LogUnrealMCP, Log, TEXT("Successfully set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                            continue;
                        }
                        else
                        {
                            const UEdGraphSchema* Schema = EventGraph->GetSchema();
                            if (!Schema)
                            {
                                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Graph schema unavailable"));
                            }
                            if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
                            {
                                Schema->TrySetDefaultText(*ParamPin, FText::FromString(StringVal));
                            }
                            else
                            {
                                Schema->TrySetDefaultValue(*ParamPin, StringVal);
                            }
                        }
                    }
                    else if (ParamValue->Type == EJson::Number)
                    {
                        // Handle integer vs float parameters correctly
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                        {
                            // Ensure we're using an integer value (no decimal)
                            int32 IntValue = FMath::RoundToInt(ParamValue->AsNumber());
                            ParamPin->DefaultValue = FString::FromInt(IntValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set integer parameter '%s' to: %d (string: '%s')"),
                                   *ParamName, IntValue, *ParamPin->DefaultValue);
                        }
                        else
                        {
                            // For other numeric types
                            float FloatValue = ParamValue->AsNumber();
                            ParamPin->DefaultValue = FString::SanitizeFloat(FloatValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set float parameter '%s' to: %f (string: '%s')"),
                                   *ParamName, FloatValue, *ParamPin->DefaultValue);
                        }
                    }
                    else if (ParamValue->Type == EJson::Boolean)
                    {
                        bool BoolValue = ParamValue->AsBool();
                        ParamPin->DefaultValue = BoolValue ? TEXT("true") : TEXT("false");
                        UE_LOG(LogTemp, Display, TEXT("  Set boolean parameter '%s' to: %s"),
                               *ParamName, *ParamPin->DefaultValue);
                    }
                    else if (ParamValue->Type == EJson::Array)
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Processing array parameter '%s'"), *ParamName);
                        // Handle array parameters - like Vector parameters
                        const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
                        if (ParamValue->TryGetArray(ArrayValue))
                        {
                            // Check if this could be a vector (array of 3 numbers)
                            if (ArrayValue->Num() == 3 &&
                                (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) &&
                                (ParamPin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get()))
                            {
                                // Create a proper vector string: (X=0.0,Y=0.0,Z=1000.0)
                                float X = (*ArrayValue)[0]->AsNumber();
                                float Y = (*ArrayValue)[1]->AsNumber();
                                float Z = (*ArrayValue)[2]->AsNumber();

                                FString VectorString = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), X, Y, Z);
                                ParamPin->DefaultValue = VectorString;

                                UE_LOG(LogTemp, Display, TEXT("  Set vector parameter '%s' to: %s"),
                                       *ParamName, *VectorString);
                                UE_LOG(LogTemp, Display, TEXT("  Final pin value: '%s'"),
                                       *ParamPin->DefaultValue);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Warning, TEXT("Array parameter type not fully supported yet"));
                            }
                        }
                    }
                    // Add handling for other types as needed
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("Parameter pin '%s' not found"), *ParamName);
                }
            }
        }
    }

    // Mark the blueprint as modified
    MCPGraphLayout::PlaceNewNode(FunctionNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), FunctionNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_type' parameter"));
    }

    // Get optional parameters
    bool IsExposed = false;
    if (Params->HasField(TEXT("is_exposed")))
    {
        IsExposed = Params->GetBoolField(TEXT("is_exposed"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Create variable based on type
    FEdGraphPinType PinType;

    // Set up pin type based on variable_type string
    if (VariableType == TEXT("Boolean"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (VariableType == TEXT("Integer") || VariableType == TEXT("Int"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (VariableType == TEXT("Float"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (VariableType == TEXT("String"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (VariableType == TEXT("Vector"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported variable type: %s"), *VariableType));
    }

    // Create the variable
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType);

    // Set variable properties
    FBPVariableDescription* NewVar = nullptr;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == FName(*VariableName))
        {
            NewVar = &Variable;
            break;
        }
    }

    if (NewVar)
    {
        // Set exposure in editor
        if (IsExposed)
        {
            NewVar->PropertyFlags |= CPF_Edit;
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("variable_type"), VariableType);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    if (!Blueprint->UbergraphPages.Contains(EventGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event nodes are only valid in Event Graphs"));
    }

    // Create the input action node
    UK2Node_InputAction* InputActionNode = FUnrealMCPCommonUtils::CreateInputActionNode(EventGraph, ActionName, NodePosition);
    if (!InputActionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create input action node"));
    }

    // Mark the blueprint as modified
    MCPGraphLayout::PlaceNewNode(InputActionNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), InputActionNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSelfReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // Create the self node
    UK2Node_Self* SelfNode = FUnrealMCPCommonUtils::CreateSelfReferenceNode(EventGraph, NodePosition);
    if (!SelfNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create self node"));
    }

    // Mark the blueprint as modified
    MCPGraphLayout::PlaceNewNode(SelfNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), SelfNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleFindBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeType;
    // 客户端在未指定类型时发送空值并用 event_type 表达具体事件；两者都接。
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
    {
        if (!Params->TryGetStringField(TEXT("event_type"), NodeType))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_type' parameter"));
        }
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the event graph
    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError, false);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // Create a JSON array for the node GUIDs
    TArray<TSharedPtr<FJsonValue>> NodeGuidArray;

    // Filter nodes by the exact requested type
    if (NodeType == TEXT("Event"))
    {
        FString EventName;
        if (!Params->TryGetStringField(TEXT("event_name"), EventName))
        {
            // 部分客户端把具体事件放在 event_type 里。
            Params->TryGetStringField(TEXT("event_type"), EventName);
        }
        if (EventName.IsEmpty())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter for Event node search"));
        }

        // Look for nodes with exact event name (e.g., ReceiveBeginPlay)
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
            {
                UE_LOG(LogTemp, Display, TEXT("Found event node with name %s: %s"), *EventName, *EventNode->NodeGuid.ToString());
                NodeGuidArray.Add(MakeShared<FJsonValueString>(EventNode->NodeGuid.ToString()));
            }
        }
    }
    // Add other node types as needed (InputAction, etc.)

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("node_guids"), NodeGuidArray);

    return ResultObj;
}
TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintCastNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString TargetClass;
    if (!Params->TryGetStringField(TEXT("target_class"), TargetClass))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_class' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // 类名支持完整路径、U 前缀与反射名；找不到时报错而不是静默替换。
    UClass* ResolvedClass = nullptr;
    if (TargetClass.StartsWith(TEXT("/Game/")) || TargetClass.StartsWith(TEXT("/Script/")))
    {
        ResolvedClass = LoadObject<UClass>(nullptr, *TargetClass);
    }
    if (!ResolvedClass)
    {
        ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *TargetClass);
    }
    if (!ResolvedClass && !TargetClass.StartsWith(TEXT("U")))
    {
        ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + TargetClass));
    }
    if (!ResolvedClass)
    {
        ResolvedClass = LoadObject<UClass>(nullptr, *(TEXT("/Script/") + TargetClass));
    }
    if (!ResolvedClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target class not found: %s"), *TargetClass));
    }

    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(EventGraph);
    CastNode->TargetType = ResolvedClass;
    CastNode->NodePosX = NodePosition.X;
    CastNode->NodePosY = NodePosition.Y;
    EventGraph->AddNode(CastNode, true);
    CastNode->CreateNewGuid();
    CastNode->PostPlacedNewNode();
    CastNode->AllocateDefaultPins();
    CastNode->ReconstructNode();

    MCPGraphLayout::PlaceNewNode(CastNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), CastNode->NodeGuid.ToString());
    ResultObj->SetStringField(TEXT("target_class"), ResolvedClass->GetPathName());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintExternalMemberGetNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString MemberClass;
    if (!Params->TryGetStringField(TEXT("member_class"), MemberClass))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'member_class' parameter"));
    }

    FString MemberName;
    if (!Params->TryGetStringField(TEXT("member_name"), MemberName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'member_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    UClass* ResolvedClass = nullptr;
    if (MemberClass.StartsWith(TEXT("/Game/")) || MemberClass.StartsWith(TEXT("/Script/")))
    {
        ResolvedClass = LoadObject<UClass>(nullptr, *MemberClass);
    }
    if (!ResolvedClass)
    {
        ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *MemberClass);
    }
    if (!ResolvedClass && !MemberClass.StartsWith(TEXT("U")))
    {
        ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + MemberClass));
    }
    if (!ResolvedClass)
    {
        ResolvedClass = LoadObject<UClass>(nullptr, *(TEXT("/Script/") + MemberClass));
    }
    if (!ResolvedClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Member class not found: %s"), *MemberClass));
    }

    // VariableGet 支持任意属性类型；这里只做存在性检查。
    FProperty* MemberProperty = ResolvedClass->FindPropertyByName(FName(*MemberName));
    if (!MemberProperty)
    {
        for (TFieldIterator<FProperty> It(ResolvedClass); It; ++It)
        {
            if (It->GetName().Equals(MemberName, ESearchCase::IgnoreCase))
            {
                MemberProperty = *It;
                break;
            }
        }
    }
    if (!MemberProperty)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Member '%s' not found on class '%s'"), *MemberName, *ResolvedClass->GetName()));
    }

    UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(EventGraph);
    GetNode->VariableReference.SetExternalMember(FName(*MemberName), ResolvedClass);
    GetNode->NodePosX = NodePosition.X;
    GetNode->NodePosY = NodePosition.Y;
    EventGraph->AddNode(GetNode, true);
    GetNode->CreateNewGuid();
    GetNode->PostPlacedNewNode();
    GetNode->AllocateDefaultPins();
    GetNode->ReconstructNode();

    MCPGraphLayout::PlaceNewNode(GetNode, Params);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph changed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), GetNode->NodeGuid.ToString());
    ResultObj->SetStringField(TEXT("member"), MemberProperty->GetName());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleGetBlueprintNodeDetails(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError, false);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node->NodeGuid.ToString() == NodeId)
        {
            TargetNode = Node;
            break;
        }
    }
    if (!TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), TargetNode->NodeGuid.ToString());
    // 节点标题便于外部识别节点用途。
    ResultObj->SetStringField(TEXT("node_title"), TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    ResultObj->SetStringField(TEXT("node_class"), TargetNode->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("pos_x"), TargetNode->NodePosX);
    ResultObj->SetNumberField(TEXT("pos_y"), TargetNode->NodePosY);

    // 输出引脚及连线；LinkedTo 只保留对端节点 ID，避免响应过大。
    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (const UEdGraphPin* Pin : TargetNode->Pins)
    {
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("pin"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("out") : TEXT("in"));
        PinObj->SetStringField(TEXT("pin_type"), Pin->PinType.PinCategory.ToString());
        // 未连接引脚的运行时行为由节点上序列化的默认值决定，必须一并输出。
        if (!Pin->DefaultValue.IsEmpty())
        {
            PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        }
        if (Pin->DefaultObject)
        {
            PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
        }
        TArray<TSharedPtr<FJsonValue>> LinkedArray;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            if (LinkedNode)
            {
                TSharedPtr<FJsonObject> LinkedObj = MakeShared<FJsonObject>();
                LinkedObj->SetStringField(TEXT("node_id"), LinkedNode->NodeGuid.ToString());
                LinkedObj->SetStringField(TEXT("node_title"), LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                LinkedObj->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
                LinkedArray.Add(MakeShared<FJsonValueObject>(LinkedObj));
            }
        }
        PinObj->SetArrayField(TEXT("linked_to"), LinkedArray);
        PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }
    ResultObj->SetArrayField(TEXT("pins"), PinsArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDeleteBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* EventGraph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!EventGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    UEdGraphNode* TargetNode = nullptr;
    for (UEdGraphNode* Node : EventGraph->Nodes)
    {
        if (Node->NodeGuid.ToString() == NodeId)
        {
            TargetNode = Node;
            break;
        }
    }
    if (!TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    // 入口事件节点不允许删除，防止把图拆坏；其余节点与编辑器手动删除行为一致。
    if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(TargetNode))
    {
        if (EventNode->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Refusing to delete the BeginPlay entry node"));
        }
    }

    if (!TargetNode->CanUserDeleteNode())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Cannot delete a protected graph entry, exit or structural node"));
    }
    const FString DeletedTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
    // RemoveNode 负责断开连线、从图中移除节点并处理 knot/comment 等清理。
    FBlueprintEditorUtils::RemoveNode(Blueprint, TargetNode, true);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        auto ErrorResult = FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint node deleted but save failed: %s"), *SaveError));
        ErrorResult->SetStringField(TEXT("deleted_node_id"), NodeId);
        return ErrorResult;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("deleted_node_id"), NodeId);
    ResultObj->SetStringField(TEXT("deleted_node_title"), DeletedTitle);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDisconnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    FString SourceNodeId;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_node_id' parameter"));
    }
    FString TargetNodeId;
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_node_id' parameter"));
    }
    FString SourcePinName;
    if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_pin' parameter"));
    }
    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_pin' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* Graph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    UEdGraphNode* SourceNode = FUnrealMCPCommonUtils::FindNodeByGuid(Graph, SourceNodeId);
    UEdGraphNode* TargetNode = FUnrealMCPCommonUtils::FindNodeByGuid(Graph, TargetNodeId);
    if (!SourceNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId));
    }
    if (!TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
    }

    if (!FUnrealMCPCommonUtils::DisconnectGraphNodes(Graph, SourceNode, SourcePinName, TargetNode, TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("No connection from '%s'.%s to '%s'.%s"),
            *SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Connection removed but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("source_node_id"), SourceNodeId);
    ResultObj->SetStringField(TEXT("source_pin"), SourcePinName);
    ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
    ResultObj->SetStringField(TEXT("target_pin"), TargetPinName);
    ResultObj->SetBoolField(TEXT("disconnected"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSetBlueprintPinDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NodeId, PinName, Value;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName) ||
        !Params->TryGetStringField(TEXT("node_id"), NodeId) ||
        !Params->TryGetStringField(TEXT("pin_name"), PinName) ||
        !Params->TryGetStringField(TEXT("default_value"), Value))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Requires blueprint_name, node_id, pin_name and default_value strings"));
    }
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    FString Error;
    UEdGraph* Graph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, Error, false);
    if (!Graph) { return FUnrealMCPCommonUtils::CreateErrorResponse(Error); }
    UEdGraphNode* Node = FUnrealMCPCommonUtils::FindNodeByGuid(Graph, NodeId);
    UEdGraphPin* Pin = FUnrealMCPCommonUtils::FindPin(Node, PinName, EGPD_Input);
    if (!Pin || Pin->LinkedTo.Num() || Pin->bDefaultValueIsReadOnly)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Requires an unconnected editable input pin"));
    }
    const UEdGraphSchema_K2* Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
    if (!Schema) { return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("K2 graph schema unavailable")); }
    FString Literal;
    UObject* DefaultObject = nullptr;
    FText DefaultText;
    Schema->GetPinDefaultValuesFromString(Pin->PinType, Node, Value, Literal, DefaultObject, DefaultText);
    Error = Schema->IsPinDefaultValid(Pin, Literal, DefaultObject, DefaultText);
    if (!Error.IsEmpty()) { return FUnrealMCPCommonUtils::CreateErrorResponse(Error); }
    Schema->TrySetDefaultValue(*Pin, Value);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("pin_name"), PinName);
    Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    Result->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
    Result->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSetBlueprintNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }
    const TSharedPtr<FJsonValue> PropertyValue = Params->TryGetField(TEXT("property_value"));
    if (!PropertyValue.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* Graph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    UEdGraphNode* TargetNode = FUnrealMCPCommonUtils::FindNodeByGuid(Graph, NodeId);
    if (!TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    // Restrict writes to editor properties, protecting node identity and graph ownership.
    FProperty* Property = TargetNode->GetClass()->FindPropertyByName(*PropertyName);
    const bool bComment = PropertyName == TEXT("NodeComment");
    if (!bComment && (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) ||
        Property->HasAnyPropertyFlags(CPF_EditConst)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Property must be an editable node property; use pin defaults for function arguments"));
    }
    if (bComment && PropertyValue->Type != EJson::String)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("NodeComment must be a string"));
    }
    // Modify() 让本次改动进入撤销栈；只开事务不改对象是撤不回的。
    Blueprint->Modify();
    TargetNode->Modify();
    FString PropertyError;
    if (bComment)
    {
        TargetNode->OnUpdateCommentText(PropertyValue->AsString());
    }
    else if (!FUnrealMCPCommonUtils::SetObjectProperty(TargetNode, PropertyName, PropertyValue, PropertyError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set '%s': %s"), *PropertyName, *PropertyError));
    }
    // 属性可能影响引脚（如 bShowPinNames），重建节点让编辑器与运行时看到一致结果。
    TargetNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    FString SaveError;
    if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node property set but save failed: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), NodeId);
    ResultObj->SetStringField(TEXT("property_name"), PropertyName);
    return ResultObj;
}

namespace
{
// 清理时永不删除的结构节点：入口/出口、事件、隧道、注释框以及引擎禁止用户删除的节点。
bool IsProtectedFromPrune(UEdGraphNode* Node)
{
    if (!Node || Node->IsA<UEdGraphNode_Comment>())
    {
        return true;
    }
    if (!Node->CanUserDeleteNode())
    {
        return true;
    }
    return Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>()
        || Node->IsA<UK2Node_FunctionResult>() || Node->IsA<UK2Node_Tunnel>();
}

bool HasAnyLinkedPin(UEdGraphNode* Node)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->LinkedTo.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

// 只统计数据输入（非执行）是否已被接入，用于区分「悬空的新调用节点」与「已接线的副作用节点」。
bool HasLinkedDataInput(UEdGraphNode* Node)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            continue;
        }
        if (Pin->LinkedTo.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

bool HasLinkedPinInDirection(UEdGraphNode* Node, EEdGraphPinDirection Direction, bool bExecOnly)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != Direction)
        {
            continue;
        }
        if (bExecOnly && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            continue;
        }
        if (Pin->LinkedTo.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

// 纯数据节点且所有输出均无人使用，才认为它不产生可观察效果。
bool IsUnusedDataNode(UEdGraphNode* Node)
{
    const UK2Node* K2Node = Cast<UK2Node>(Node);
    if (!K2Node || !K2Node->IsNodePure())
    {
        return false;
    }
    return !HasLinkedPinInDirection(Node, EGPD_Output, false);
}
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandlePruneUnusedNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }
    const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
    if (!Params->TryGetArrayField(TEXT("candidate_node_ids"), Ids) || !Ids)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'candidate_node_ids' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }
    FString GraphError;
    UEdGraph* Graph = FUnrealMCPCommonUtils::ResolveGraphForCommand(Blueprint, Params, GraphError, false);
    if (!Graph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError.IsEmpty() ? TEXT("Failed to get event graph") : GraphError);
    }

    // 候选集合只包含本次操作新增的节点；向上游递归不会越过这个集合。
    TSet<UEdGraphNode*> Candidates;
    for (const TSharedPtr<FJsonValue>& Entry : *Ids)
    {
        FString Value;
        if (!Entry.IsValid() || !Entry->TryGetString(Value))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("candidate_node_ids must contain strings"));
        }
        if (UEdGraphNode* Found = FUnrealMCPCommonUtils::FindNodeByGuid(Graph, Value))
        {
            Candidates.Add(Found);
        }
    }

    TArray<TSharedPtr<FJsonValue>> Removed;
    TArray<TSharedPtr<FJsonValue>> Kept;
    TArray<TSharedPtr<FJsonValue>> Messages;

    // 反复扫描直到本轮没有删除，才能覆盖「删下游后上游变成无用」的链式情况。
    // 每轮先定下「必须保留」的集合，再清其余候选，避免误删被保留节点正在使用的数据节点。
    bool bRemovedAny = true;
    while (bRemovedAny)
    {
        bRemovedAny = false;

        // 第一遍：找出本轮必须保留的候选——接入执行链的副作用节点，以及被别人使用的节点。
        TSet<UEdGraphNode*> MustKeep;
        for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes))
        {
            if (!Candidates.Contains(Node))
            {
                continue;
            }
            const UK2Node* K2Node = Cast<UK2Node>(Node);
            const bool bPure = K2Node && K2Node->IsNodePure();
            if (!bPure && HasLinkedPinInDirection(Node, EGPD_Input, true))
            {
                // 执行输入已连接：属于执行链，即使输出悬空也必须保留。
                MustKeep.Add(Node);
            }
            if (HasLinkedPinInDirection(Node, EGPD_Output, false) || HasLinkedPinInDirection(Node, EGPD_Output, true))
            {
                // 有任何输出被使用（数据或执行），自身有用途。
                MustKeep.Add(Node);
            }
        }
        // 让保留向下游传递：被保留节点所消费的上游数据节点同样保留。
        for (UEdGraphNode* Keep : TArray<UEdGraphNode*>(MustKeep.Array()))
        {
            for (UEdGraphPin* Pin : Keep->Pins)
            {
                if (Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Upstream = Linked ? Linked->GetOwningNode() : nullptr;
                    if (Upstream && Candidates.Contains(Upstream))
                    {
                        MustKeep.Add(Upstream);
                    }
                }
            }
        }

        TArray<UEdGraphNode*> Snapshot = Graph->Nodes;
        for (UEdGraphNode* Node : Snapshot)
        {
            // 每轮重新评估全部候选：删掉下游后，上游可能在本轮变成无用节点。
            if (!Candidates.Contains(Node) || !IsValid(Node))
            {
                continue;
            }

            if (IsProtectedFromPrune(Node))
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
                Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Item->SetStringField(TEXT("reason"), TEXT("protected structural or entry node"));
                Kept.Add(MakeShared<FJsonValueObject>(Item));
                continue;
            }

            const UK2Node* K2Node = Cast<UK2Node>(Node);
            const bool bPure = K2Node && K2Node->IsNodePure();
            // 保留集合内的节点一律不动；其余候选才进入清理判定。
            bool bRemove = !MustKeep.Contains(Node);
            if (bRemove && !bPure)
            {
                // 副作用节点只在完全未接线时才算悬空；接线过的一律保留。
                bRemove = !HasAnyLinkedPin(Node);
            }

            if (bRemove)
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
                Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Removed.Add(MakeShared<FJsonValueObject>(Item));
                Candidates.Remove(Node);
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                bRemovedAny = true;
                continue;
            }

            // 仍有输入连线或含有副作用的节点：保留，并把上游候选一并保留。
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetStringField(TEXT("reason"),
                !bPure ? TEXT("impure node is attached to an execution chain")
                       : TEXT("still used by a consumer"));
            Kept.Add(MakeShared<FJsonValueObject>(Item));
        }
    }

    if (Removed.Num() > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        FString SaveError;
        if (!FUnrealMCPCommonUtils::SaveUnlessDeferred(Blueprint, Params, SaveError))
        {
            auto Failure = FUnrealMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unused nodes removed but save failed: %s"), *SaveError));
            Failure->SetArrayField(TEXT("removed"), Removed);
            Failure->SetArrayField(TEXT("kept"), Kept);
            return Failure;
        }
    }
    else
    {
        Messages.Add(MakeShared<FJsonValueString>(TEXT("No unused nodes to remove")));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("removed"), Removed);
    ResultObj->SetArrayField(TEXT("kept"), Kept);
    ResultObj->SetArrayField(TEXT("messages"), Messages);
    ResultObj->SetNumberField(TEXT("removed_count"), Removed.Num());
    return ResultObj;
}
