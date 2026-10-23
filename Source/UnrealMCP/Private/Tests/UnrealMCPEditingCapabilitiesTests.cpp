#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPMaterialCommands.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPEditingCapabilitiesTest,
    "UnrealMCP.Editing.GraphScopeAndSingleLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPEditingCapabilitiesTest::RunTest(const FString& Parameters)
{
    const FString Name = TEXT("BP_Capabilities_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = TEXT("/Game/UnrealMCPCapabilityTests/") + Name;
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        CreatePackage(*Path), FName(*Name), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }
    ON_SCOPE_EXIT { Blueprint->ClearFlags(RF_Public | RF_Standalone); Blueprint->MarkPendingKill(); };
    FUnrealMCPBlueprintNodeCommands Commands;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("blueprint_name"), Blueprint->GetPathName());
    Params->SetBoolField(TEXT("defer_save"), true);
    Params->SetStringField(TEXT("graph_name"), TEXT("Calculate"));
    Params->SetStringField(TEXT("graph_type"), TEXT("function"));
    auto Result = Commands.HandleCommand(TEXT("create_blueprint_graph"), Params);
    if (!TestFalse(TEXT("Function graph created"), Result->HasField(TEXT("error")))) { return false; }
    UEdGraph* Graph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, TEXT("Calculate"));
    if (!TestNotNull(TEXT("Function graph addressable"), Graph)) { return false; }
    const int32 Count = Blueprint->FunctionGraphs.Num();
    TestTrue(TEXT("Duplicate graph rejected"), Commands.HandleCommand(TEXT("create_blueprint_graph"), Params)->HasField(TEXT("error")));
    TestEqual(TEXT("Duplicate did not add a graph"), Blueprint->FunctionGraphs.Num(), Count);
    Params->SetStringField(TEXT("graph_name"), TEXT("MyMacro"));
    Params->SetStringField(TEXT("graph_type"), TEXT("macro"));
    TestFalse(TEXT("Macro graph created"), Commands.HandleCommand(TEXT("create_blueprint_graph"), Params)->HasField(TEXT("error")));
    TestNotNull(TEXT("Macro graph addressable"), FUnrealMCPCommonUtils::FindGraphByName(Blueprint, TEXT("MyMacro")));
    TestFalse(TEXT("Deferred edits have not created a disk package"), FPackageName::DoesPackageExist(Path));
    Params->SetStringField(TEXT("graph_name"), TEXT("Calculate"));
    const int32 StructuralCount = Graph->Nodes.Num();
    Params->SetStringField(TEXT("node_id"), Graph->Nodes[0]->NodeGuid.ToString());
    TestTrue(TEXT("Function entry cannot be deleted"), Commands.HandleCommand(TEXT("delete_blueprint_node"), Params)->HasField(TEXT("error")));
    TestEqual(TEXT("Entry remains in graph"), Graph->Nodes.Num(), StructuralCount);

    UFunction* Add = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Add_FloatFloat"));
    if (!TestNotNull(TEXT("Float addition reflected"), Add)) { return false; }
    auto Source = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, Add, FVector2D(0, 0));
    auto First = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, Add, FVector2D(300, 0));
    auto Second = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, Add, FVector2D(300, 200));
    if (!Source || !First || !Second) { return false; }
    TestTrue(TEXT("First data link"), FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, Source, TEXT("ReturnValue"), First, TEXT("A")));
    TestTrue(TEXT("Sibling data link"), FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, Source, TEXT("ReturnValue"), Second, TEXT("A")));
    TestTrue(TEXT("Only requested link removed"), FUnrealMCPCommonUtils::DisconnectGraphNodes(Graph, Source, TEXT("ReturnValue"), First, TEXT("A")));
    UEdGraphPin* Output = FUnrealMCPCommonUtils::FindPin(Source, TEXT("ReturnValue"));
    UEdGraphPin* Remaining = FUnrealMCPCommonUtils::FindPin(Second, TEXT("A"), EGPD_Input);
    TestTrue(TEXT("Sibling link survives"), Output && Remaining && Output->LinkedTo.Contains(Remaining));

    Params->SetStringField(TEXT("graph_name"), TEXT("Calculate"));
    Params->SetStringField(TEXT("node_id"), First->NodeGuid.ToString());
    Params->SetStringField(TEXT("pin_name"), TEXT("B"));
    Params->SetStringField(TEXT("default_value"), TEXT("3.5"));
    TestFalse(TEXT("Function pin default editable"), Commands.HandleCommand(TEXT("set_blueprint_pin_default"), Params)->HasField(TEXT("error")));
    TestEqual(TEXT("Literal changed"), FUnrealMCPCommonUtils::FindPin(First, TEXT("B"), EGPD_Input)->DefaultValue, FString(TEXT("3.5")));
    Params->SetStringField(TEXT("property_name"), TEXT("NodeComment"));
    Params->SetStringField(TEXT("property_value"), TEXT("Reviewed comment"));
    TestFalse(TEXT("Comment editable without CPF_Edit"), Commands.HandleCommand(TEXT("set_blueprint_node_property"), Params)->HasField(TEXT("error")));
    TestEqual(TEXT("Comment changed"), First->NodeComment, FString(TEXT("Reviewed comment")));
    UFunction* FindActor = UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("GetActorOfClass"));
    auto FindActorNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, FindActor, FVector2D(600, 0));
    if (!TestNotNull(TEXT("Class input node"), FindActorNode)) { return false; }
    Params->SetStringField(TEXT("node_id"), FindActorNode->NodeGuid.ToString());
    Params->SetStringField(TEXT("pin_name"), TEXT("ActorClass"));
    Params->SetStringField(TEXT("default_value"), TEXT("/Script/Engine.Actor"));
    TestFalse(TEXT("Class default parsed before validation"), Commands.HandleCommand(TEXT("set_blueprint_pin_default"), Params)->HasField(TEXT("error")));
    TestTrue(TEXT("Class default resolves to reflected class"),
        FUnrealMCPCommonUtils::FindPin(FindActorNode, TEXT("ActorClass"), EGPD_Input)->DefaultObject == AActor::StaticClass());
    TestFalse(TEXT("Function graph introspection"), Commands.HandleCommand(TEXT("get_blueprint_graph"), Params)->HasField(TEXT("error")));

    // A lookup must not manufacture an Event Graph on a Blueprint without one.
    const auto Pages = Blueprint->UbergraphPages;
    Blueprint->UbergraphPages.Reset();
    Params->RemoveField(TEXT("graph_name"));
    TestTrue(TEXT("Missing event graph returns error"), Commands.HandleCommand(TEXT("get_blueprint_graph"), Params)->HasField(TEXT("error")));
    TestEqual(TEXT("Read did not create a graph"), Blueprint->UbergraphPages.Num(), 0);
    Blueprint->UbergraphPages = Pages;
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPMaterialWorldIsolationTest,
    "UnrealMCP.Materials.PIEWorldIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPMaterialWorldIsolationTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld)
    {
        AddWarning(TEXT("Requires an editor without an active PIE session"));
        return true;
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!TestNotNull(TEXT("Editor world"), World)) { return false; }
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
    if (!TestNotNull(TEXT("Editor actor"), Actor)) { return false; }
    ON_SCOPE_EXIT { World->DestroyActor(Actor); };
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor_name"), Actor->GetName());
    Params->SetStringField(TEXT("component_name"), Actor->GetStaticMeshComponent()->GetName());
    FUnrealMCPMaterialCommands Commands;
    Params->SetBoolField(TEXT("pie"), false);
    TestFalse(TEXT("Editor lookup succeeds"), Commands.HandleCommand(TEXT("get_component_materials"), Params)->HasField(TEXT("error")));
    Params->SetBoolField(TEXT("pie"), true);
    TestTrue(TEXT("PIE lookup never falls back to editor world"), Commands.HandleCommand(TEXT("get_component_materials"), Params)->HasField(TEXT("error")));
    return true;
}
#endif
