#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPPruneUnusedTest,
    "UnrealMCP.Editing.PruneUnusedNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPruneUnusedTest::RunTest(const FString& Parameters)
{
    const FString Name = TEXT("BP_Prune_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = TEXT("/Game/UnrealMCPPruneTests/") + Name;
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        CreatePackage(*Path), FName(*Name), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }
    ON_SCOPE_EXIT
    {
        // 不清理磁盘资产会让后续运行被 "asset already exists" 挡住
        if (UEditorAssetLibrary::DoesAssetExist(Path))
        {
            UEditorAssetLibrary::DeleteAsset(Path);
        }
        Blueprint->ClearFlags(RF_Public | RF_Standalone);
        Blueprint->MarkPendingKill();
    };

    UEdGraph* Graph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    if (!TestNotNull(TEXT("Event graph"), Graph)) { return false; }

    UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
    UFunction* AddFloat = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Add_FloatFloat"));
    if (!TestNotNull(TEXT("PrintString"), PrintString) || !TestNotNull(TEXT("Add_FloatFloat"), AddFloat)) { return false; }

    UK2Node_Event* BeginPlay = FUnrealMCPCommonUtils::CreateEventNode(Graph, TEXT("ReceiveBeginPlay"), FVector2D::ZeroVector);
    if (!TestNotNull(TEXT("BeginPlay"), BeginPlay)) { return false; }

    // A：已接入执行链的副作用节点（执行输出悬空）——必须保留
    UK2Node_CallFunction* Attached = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, PrintString, FVector2D(0, 0));
    // B：完全未接线的副作用节点——按约定清理
    UK2Node_CallFunction* Dangling = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, PrintString, FVector2D(0, 0));
    // C：末端纯数据节点，输入仍连着但输出无人使用——必须清理
    UK2Node_CallFunction* PureLeaf = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, AddFloat, FVector2D(0, 0));
    // D：上游纯数据节点，仅被 C 使用——删掉 C 后必须被链式清理
    UK2Node_CallFunction* PureUpstream = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, AddFloat, FVector2D(0, 0));
    // E：被保留节点使用的纯数据节点——必须保留
    UK2Node_CallFunction* PureUsed = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, AddFloat, FVector2D(0, 0));
    if (!Attached || !Dangling || !PureLeaf || !PureUpstream || !PureUsed) { return false; }
    TestTrue(TEXT("Pure nodes are pure"), PureLeaf->IsNodePure() && PureUpstream->IsNodePure() && PureUsed->IsNodePure());
    TestFalse(TEXT("PrintString node is impure"), Attached->IsNodePure());

    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, BeginPlay, TEXT("then"), Attached, TEXT("execute"));
    // C 的输入接上游，输出完全不接
    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, PureUpstream, TEXT("ReturnValue"), PureLeaf, TEXT("A"));
    // Attached 的 InString 接一个纯数据节点，使它可以存活
    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, PureUsed, TEXT("ReturnValue"), Attached, TEXT("InString"));

    const int32 Before = Graph->Nodes.Num();
    const FString AttachedId = Attached->NodeGuid.ToString();
    const FString DanglingId = Dangling->NodeGuid.ToString();
    const FString LeafId = PureLeaf->NodeGuid.ToString();
    const FString UpstreamId = PureUpstream->NodeGuid.ToString();
    const FString UsedId = PureUsed->NodeGuid.ToString();
    const FString BeginId = BeginPlay->NodeGuid.ToString();

    FUnrealMCPBlueprintNodeCommands Commands;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("blueprint_name"), Path);
    Params->SetBoolField(TEXT("defer_save"), true);
    auto Ids = [](std::initializer_list<FString> In)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : In)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    };
    // 事件节点不在候选内：即使它没有输出也不得被动
    Params->SetArrayField(TEXT("candidate_node_ids"),
        Ids({ AttachedId, DanglingId, LeafId, UpstreamId, UsedId }));

    const TSharedPtr<FJsonObject> Result = Commands.HandleCommand(TEXT("prune_unused_nodes"), Params);
    if (!TestFalse(TEXT("Prune succeeded"), Result->HasField(TEXT("error")))) { return false; }

    // 用 GUID 判定：节点删除后原指针不可靠，Contains 会误判
    auto Present = [Graph](const FString& Guid)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid.ToString() == Guid)
            {
                return true;
            }
        }
        return false;
    };
    TestTrue(TEXT("Attached side-effect node kept"), Present(AttachedId));
    TestFalse(TEXT("Dangling side-effect node removed"), Present(DanglingId));
    TestTrue(TEXT("Pure leaf removed despite wired input"), !Present(LeafId));
    TestTrue(TEXT("Upstream pruned transitively"), !Present(UpstreamId));
    TestTrue(TEXT("Used pure node kept"), Present(UsedId));
    TestTrue(TEXT("Event node untouched"), Present(BeginId));
    TestFalse(TEXT("Dangling GUID gone"), Present(DanglingId));
    TestFalse(TEXT("Leaf GUID gone"), Present(LeafId));
    TestFalse(TEXT("Upstream GUID gone"), Present(UpstreamId));
    TestTrue(TEXT("Attached GUID present"), Present(AttachedId));
    TestTrue(TEXT("Used GUID present"), Present(UsedId));
    TestTrue(TEXT("BeginPlay GUID present"), Present(BeginId));

    const int32 RemovedCount = (int32)Result->GetNumberField(TEXT("removed_count"));
    TestEqual(TEXT("Three nodes removed"), RemovedCount, 3);
    TestEqual(TEXT("Graph lost exactly three nodes"), Before - Graph->Nodes.Num(), 3);

    // 保留项必须带原因，便于调用方复核
    const TArray<TSharedPtr<FJsonValue>>* Kept = nullptr;
    if (TestTrue(TEXT("Kept list reported"), Result->TryGetArrayField(TEXT("kept"), Kept)))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Kept)
        {
            const TSharedPtr<FJsonObject> Item = Entry->AsObject();
            TestTrue(TEXT("Kept entry carries a reason"),
                Item.IsValid() && !Item->GetStringField(TEXT("reason")).IsEmpty());
        }
    }

    // 重复调用必须幂等：没有候选后不再删除
    auto Again = MakeShared<FJsonObject>();
    Again->SetStringField(TEXT("blueprint_name"), Path);
    Again->SetBoolField(TEXT("defer_save"), true);
    Again->SetArrayField(TEXT("candidate_node_ids"), Ids({ AttachedId, UsedId }));
    const TSharedPtr<FJsonObject> Second = Commands.HandleCommand(TEXT("prune_unused_nodes"), Again);
    TestFalse(TEXT("Second prune succeeded"), Second->HasField(TEXT("error")));
    TestEqual(TEXT("Second prune removed nothing"),
        (int32)Second->GetNumberField(TEXT("removed_count")), 0);

    return !HasAnyErrors();
}
#endif
