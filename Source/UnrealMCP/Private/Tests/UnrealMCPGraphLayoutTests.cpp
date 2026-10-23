#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPGraphLayoutTest, "UnrealMCP.Layout.GraphPreservesConnections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPGraphLayoutTest::RunTest(const FString& Parameters)
{
    const FString Path = TEXT("/Game/UnrealMCPLayoutTests/BP_Layout");
    if (UEditorAssetLibrary::DoesAssetExist(Path))
    {
        AddError(TEXT("Test asset already exists; refusing to replace it"));
        return false;
    }
    UPackage* Package = CreatePackage(*Path);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
        TEXT("BP_Layout"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!Blueprint)
    {
        return false;
    }
    ON_SCOPE_EXIT { UEditorAssetLibrary::DeleteAsset(Path); };
    UEdGraph* Graph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    if (!Graph) { return false; }
    UK2Node_Event* Event = FUnrealMCPCommonUtils::CreateEventNode(Graph, TEXT("ReceiveBeginPlay"), FVector2D::ZeroVector);
    UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
    UK2Node_CallFunction* Call = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, Function, FVector2D::ZeroVector);
    if (!Event)
    {
        return false;
    }
    if (!Call)
    {
        return false;
    }
    TestTrue(TEXT("Execution nodes connected"), FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, Event, TEXT("then"), Call, TEXT("execute")));
    // Actor blueprint factories may seed Tick and overlap events; this case must create Tick itself.
    const TArray<UEdGraphNode*> InitialNodes = Graph->Nodes;
    for (UEdGraphNode* Node : InitialNodes)
    {
        if (Node != Event && Node != Call) Node->DestroyNode();
    }
    TestNull(TEXT("Tick is absent before the creation case"),
        FUnrealMCPCommonUtils::FindExistingEventNode(Graph, TEXT("ReceiveTick")));
    const int32 NodeCount = Graph->Nodes.Num();
    FUnrealMCPBlueprintNodeCommands Commands;
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("blueprint_name"), Path);
    Params->SetBoolField(TEXT("whole_graph"), true);
    TSharedPtr<FJsonObject> Result = Commands.HandleCommand(TEXT("format_blueprint_graph"), Params);
    TestFalse(TEXT("Formatter succeeded"), Result->HasField(TEXT("error")));
    TestTrue(TEXT("Execution moves left to right"), Event->NodePosX < Call->NodePosX);
    TestEqual(TEXT("Execution stays aligned"), Event->NodePosY, Call->NodePosY);
    TestEqual(TEXT("Nodes preserved"), Graph->Nodes.Num(), NodeCount);
    const FVector2D First(Event->NodePosX, Event->NodePosY);
    const FVector2D Second(Call->NodePosX, Call->NodePosY);
    Commands.HandleCommand(TEXT("format_blueprint_graph"), Params);
    TestEqual(TEXT("Event placement stable"), FVector2D(Event->NodePosX, Event->NodePosY), First);
    TestEqual(TEXT("Call placement stable"), FVector2D(Call->NodePosX, Call->NodePosY), Second);
    UEdGraphPin* Output = FUnrealMCPCommonUtils::FindPin(Event, TEXT("then"), EGPD_Output);
    UEdGraphPin* Input = FUnrealMCPCommonUtils::FindPin(Call, TEXT("execute"), EGPD_Input);
    TestTrue(TEXT("Connection preserved"), Output->LinkedTo.Contains(Input));
    auto Tick = MakeShared<FJsonObject>();
    Tick->SetStringField(TEXT("blueprint_name"), Path);
    Tick->SetStringField(TEXT("event_name"), TEXT("ReceiveTick"));
    TSharedPtr<FJsonObject> TickResult = Commands.HandleCommand(TEXT("add_blueprint_event_node"), Tick);
    TestFalse(TEXT("Handler placement succeeded"), TickResult->HasField(TEXT("error")));
    UK2Node_Event* TickNode = FUnrealMCPCommonUtils::FindExistingEventNode(Graph, TEXT("ReceiveTick"));
    if (!TickNode)
    {
        AddError(TEXT("Tick node missing after handler call"));
        return false;
    }
    TestFalse(TEXT("Auto-placement case creates a new event"), TickResult->GetBoolField(TEXT("already_exists")));
    TestEqual(TEXT("Auto-placement case adds exactly one node"), Graph->Nodes.Num(), NodeCount + 1);
    TestTrue(TEXT("Auto placement leaves origin"), TickNode->NodePosX != 0 || TickNode->NodePosY != 0);
    TestFalse(TEXT("Placed node does not overlap executed chain"),
        FMath::Abs(TickNode->NodePosY - Event->NodePosY) < 96.0f && TickNode->NodePosX < Call->NodePosX + 320.0f);
    auto Fixed = MakeShared<FJsonObject>();
    Fixed->SetStringField(TEXT("blueprint_name"), Path);
    Fixed->SetStringField(TEXT("event_name"), TEXT("ReceiveEndPlay"));
    TArray<TSharedPtr<FJsonValue>> FixedPosition = {MakeShared<FJsonValueNumber>(1234.0), MakeShared<FJsonValueNumber>(4321.0)};
    Fixed->SetArrayField(TEXT("node_position"), FixedPosition);
    TestFalse(TEXT("Explicit position accepted"), Commands.HandleCommand(TEXT("add_blueprint_event_node"), Fixed)->HasField(TEXT("error")));
    const FVector2D TickPosition(TickNode->NodePosX, TickNode->NodePosY);
    auto ExistingTick = Commands.HandleCommand(TEXT("add_blueprint_event_node"), Tick);
    TestTrue(TEXT("Repeated event request reports the existing node"), ExistingTick->GetBoolField(TEXT("already_exists")));
    TestEqual(TEXT("Repeated event request preserves its position"),
        FVector2D(TickNode->NodePosX, TickNode->NodePosY), TickPosition);
    TestEqual(TEXT("Repeated event request does not create a duplicate"), Graph->Nodes.Num(), NodeCount + 2);
    UK2Node_Event* EndPlayNode = FUnrealMCPCommonUtils::FindExistingEventNode(Graph, TEXT("ReceiveEndPlay"));
    TestTrue(TEXT("Explicit position respected"), EndPlayNode && EndPlayNode->NodePosX == 1234 && EndPlayNode->NodePosY == 4321);
    TSharedPtr<FJsonObject> Invalid = MakeShared<FJsonObject>();
    Invalid->SetStringField(TEXT("blueprint_name"), Path);
    TArray<TSharedPtr<FJsonValue>> Ids;
    Ids.Add(MakeShared<FJsonValueString>(TEXT("not-a-node")));
    Invalid->SetArrayField(TEXT("node_ids"), Ids);
    TestTrue(TEXT("Unknown selection rejected"), Commands.HandleCommand(TEXT("format_blueprint_graph"), Invalid)->HasField(TEXT("error")));
    return !HasAnyErrors();
}
#endif
