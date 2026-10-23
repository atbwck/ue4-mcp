#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "Misc/ScopeExit.h"

namespace
{
    struct FLayoutRect
    {
        FString Guid;
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    // Describe 的 node_id 是 GUID，用它把几何映射回测试持有的节点。
    void ReadRects(const TSharedPtr<FJsonObject>& Described, TMap<FString, FLayoutRect>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Described.IsValid() || !Described->TryGetArrayField(TEXT("nodes"), Nodes))
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Nodes)
        {
            const TSharedPtr<FJsonObject> Item = Entry->AsObject();
            FLayoutRect Rect;
            Rect.Guid = Item->GetStringField(TEXT("node_id"));
            Rect.X = (float)Item->GetNumberField(TEXT("x"));
            Rect.Y = (float)Item->GetNumberField(TEXT("y"));
            Rect.W = (float)Item->GetNumberField(TEXT("width"));
            Rect.H = (float)Item->GetNumberField(TEXT("height"));
            Out.Add(Rect.Guid, Rect);
        }
    }

    bool Overlap(const FLayoutRect& A, const FLayoutRect& B)
    {
        return A.X < B.X + B.W && A.X + A.W > B.X && A.Y < B.Y + B.H && A.Y + A.H > B.Y;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPFlowLayoutTest,
    "UnrealMCP.Layout.NoOverlapPerFlowBands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPFlowLayoutTest::RunTest(const FString& Parameters)
{
    const FString Name = TEXT("BP_FlowLayout_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = TEXT("/Game/UnrealMCPLayoutTests/") + Name;
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        CreatePackage(*Path), FName(*Name), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }
    // 必须在磁盘上也删掉，否则下一次运行会被"asset already exists"挡住
    ON_SCOPE_EXIT
    {
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

    // 两条互不相连的执行流，各带一个分支，外加一条数据链
    UK2Node_Event* BeginPlay = FUnrealMCPCommonUtils::CreateEventNode(Graph, TEXT("ReceiveBeginPlay"), FVector2D::ZeroVector);
    UK2Node_Event* Tick = FUnrealMCPCommonUtils::CreateEventNode(Graph, TEXT("ReceiveTick"), FVector2D::ZeroVector);
    UK2Node_CallFunction* PrintA = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, PrintString, FVector2D::ZeroVector);
    UK2Node_CallFunction* PrintB = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, PrintString, FVector2D::ZeroVector);
    UK2Node_CallFunction* PrintC = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, PrintString, FVector2D::ZeroVector);
    UK2Node_CallFunction* Math = FUnrealMCPCommonUtils::CreateFunctionCallNode(Graph, AddFloat, FVector2D::ZeroVector);
    if (!BeginPlay || !Tick || !PrintA || !PrintB || !PrintC || !Math) { return false; }

    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, BeginPlay, TEXT("then"), PrintA, TEXT("execute"));
    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, PrintA, TEXT("then"), PrintB, TEXT("execute"));
    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, Tick, TEXT("then"), PrintC, TEXT("execute"));
    FUnrealMCPCommonUtils::ConnectGraphNodes(Graph, Math, TEXT("ReturnValue"), PrintA, TEXT("InString"));

    // 蓝图自带的事件节点位置不受创建参数控制，这里显式拉开，确保排版前是"杂乱"的
    for (UEdGraphNode* Node : { (UEdGraphNode*)BeginPlay, (UEdGraphNode*)Tick, (UEdGraphNode*)PrintA,
                                (UEdGraphNode*)PrintB, (UEdGraphNode*)PrintC, (UEdGraphNode*)Math })
    {
        Node->NodePosX = 4321;
        Node->NodePosY = 4321;
    }

    // 手工注释框：排版必须绕开它，且不得移动它
    UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
    Comment->NodePosX = 0;
    Comment->NodePosY = 0;
    Comment->NodeWidth = 400;
    Comment->NodeHeight = 300;
    Graph->AddNode(Comment, true);

    FUnrealMCPBlueprintNodeCommands Commands;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("blueprint_name"), Path);
    Params->SetBoolField(TEXT("whole_graph"), true);
    Params->SetBoolField(TEXT("dry_run"), true);

    const TSharedPtr<FJsonObject> Plan = Commands.HandleCommand(TEXT("format_blueprint_graph"), Params);
    if (!TestFalse(TEXT("Dry run succeeded"), Plan->HasField(TEXT("error")))) { return false; }
    TestEqual(TEXT("Dry run left nodes alone"), BeginPlay->NodePosX, 4321);
    TestTrue(TEXT("Dry run planned every node"), (int32)Plan->GetNumberField(TEXT("formatted_count")) >= 6);

    Params->SetBoolField(TEXT("dry_run"), false);
    const TSharedPtr<FJsonObject> Result = Commands.HandleCommand(TEXT("format_blueprint_graph"), Params);
    if (!TestFalse(TEXT("Format succeeded"), Result->HasField(TEXT("error")))) { return false; }
    TestTrue(TEXT("Format actually moved nodes"), BeginPlay->NodePosX != 4321);

    TMap<FString, FLayoutRect> Rects;
    ReadRects(Result, Rects);
    if (!TestTrue(TEXT("Geometry reported"), Rects.Num() >= 6)) { return false; }

    // 只取执行链成员：数据节点单独用下面的断言校验是否留在本带内
    const FString Flow1[] = { BeginPlay->NodeGuid.ToString(), PrintA->NodeGuid.ToString(),
                              PrintB->NodeGuid.ToString() };
    const FString Flow2[] = { Tick->NodeGuid.ToString(), PrintC->NodeGuid.ToString() };

    // 验收 1：任意两个非注释节点不重叠
    TArray<FLayoutRect> NodeRects;
    for (const auto& Pair : Rects)
    {
        if (Pair.Key != Comment->NodeGuid.ToString())
        {
            NodeRects.Add(Pair.Value);
        }
    }
    for (int32 i = 0; i < NodeRects.Num(); ++i)
    {
        for (int32 j = i + 1; j < NodeRects.Num(); ++j)
        {
            if (Overlap(NodeRects[i], NodeRects[j]))
            {
                AddError(FString::Printf(TEXT("Nodes overlap: %s / %s"), *NodeRects[i].Guid, *NodeRects[j].Guid));
            }
        }
    }

    // 验收 2：两条执行流各占独立水平带，Y 区间不相交
    auto BandOf = [&Rects](const FString* Guids, int32 Count, float& OutTop, float& OutBottom)
    {
        OutTop = TNumericLimits<float>::Max();
        OutBottom = -TNumericLimits<float>::Max();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (const FLayoutRect* Rect = Rects.Find(Guids[Index]))
            {
                OutTop = FMath::Min(OutTop, Rect->Y);
                OutBottom = FMath::Max(OutBottom, Rect->Y + Rect->H);
            }
        }
    };
    float Top1 = 0, Bottom1 = 0, Top2 = 0, Bottom2 = 0;
    BandOf(Flow1, UE_ARRAY_COUNT(Flow1), Top1, Bottom1);
    BandOf(Flow2, UE_ARRAY_COUNT(Flow2), Top2, Bottom2);
    TestTrue(TEXT("Flow bands do not intersect"),
        Bottom1 <= Top2 || Bottom2 <= Top1);
    // 数据节点不得落进另一条流的带（允许在本带内任意位置）
    if (const FLayoutRect* MathRect = Rects.Find(Math->NodeGuid.ToString()))
    {
        const bool bInsideFlow2 = MathRect->Y < Bottom2 && MathRect->Y + MathRect->H > Top2;
        TestFalse(TEXT("Data node does not intrude into the other flow band"), bInsideFlow2);
    }

    // 验收 3：同一列共 X（同流内后一列严格在右）
    TestTrue(TEXT("Chain advances left to right"), PrintA->NodePosX < PrintB->NodePosX);
    TestTrue(TEXT("Second flow restarts at the left edge"), Tick->NodePosX < PrintC->NodePosX);
    TestEqual(TEXT("Both flow entries share the first column"), BeginPlay->NodePosX, Tick->NodePosX);

    // 验收 4：注释框既不被移动，也没有节点落在里面
    TestEqual(TEXT("Comment untouched X"), Comment->NodePosX, 0);
    TestEqual(TEXT("Comment untouched Y"), Comment->NodePosY, 0);
    TestEqual(TEXT("Comment size untouched"), Comment->NodeHeight, 300);
    const FLayoutRect Box{ Comment->NodeGuid.ToString(), 0.0f, 0.0f, 400.0f, 300.0f };
    for (const FLayoutRect& Rect : NodeRects)
    {
        if (Overlap(Rect, Box))
        {
            AddError(FString::Printf(TEXT("Node placed inside comment box: %s"), *Rect.Guid));
        }
    }

    // mode=unplaced 只整理原点节点，不动已摆放的
    const int32 PinnedX = PrintC->NodePosX;
    auto Unplaced = MakeShared<FJsonObject>();
    Unplaced->SetStringField(TEXT("blueprint_name"), Path);
    Unplaced->SetStringField(TEXT("mode"), TEXT("unplaced"));
    const TSharedPtr<FJsonObject> UnplacedResult = Commands.HandleCommand(TEXT("format_blueprint_graph"), Unplaced);
    TestFalse(TEXT("Unplaced mode succeeded"), UnplacedResult->HasField(TEXT("error")));
    TestEqual(TEXT("Unplaced mode left positioned nodes alone"), PrintC->NodePosX, PinnedX);
    TestEqual(TEXT("Unplaced mode found nothing to move"),
        (int32)UnplacedResult->GetNumberField(TEXT("formatted_count")), 0);

    return !HasAnyErrors();
}
#endif
