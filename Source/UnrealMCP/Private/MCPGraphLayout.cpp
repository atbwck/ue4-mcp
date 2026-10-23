#include "MCPGraphLayout.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "EdGraphNode_Comment.h"

namespace MCPGraphLayout
{
    namespace
    {
        const int32 Grid = 16;
        const float Gap = 64.0f;
        // 独立执行流之间额外留出的空行，避免两条流在视觉上粘连
        const float FlowSeperation = 160.0f;
        // 数据链每列相对执行列的固定偏移，保证同深度共列
        const float DataColumnStep = 320.0f;

        int32 Snap(float Value)
        {
            return FMath::CeilToInt(Value / Grid) * Grid;
        }

        bool Overlaps(const FVector2D& Position, const FVector2D& Size, UEdGraphNode* Other)
        {
            bool bEstimated = false;
            const FVector2D OtherSize = Measure(Other, bEstimated);
            return Position.X < Other->NodePosX + OtherSize.X + Gap && Position.X + Size.X + Gap > Other->NodePosX
                && Position.Y < Other->NodePosY + OtherSize.Y + Gap && Position.Y + Size.Y + Gap > Other->NodePosY;
        }

        bool HasExecution(UEdGraphNode* Node)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    return true;
                }
            }
            return false;
        }

        bool IsExecPin(const UEdGraphPin* Pin)
        {
            return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
        }

        TArray<UEdGraphNode*> Neighbours(UEdGraphNode* Node, EEdGraphPinDirection Direction, bool bExecution)
        {
            TArray<UEdGraphNode*> Result;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->Direction != Direction)
                {
                    continue;
                }
                if (IsExecPin(Pin) != bExecution)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    Result.AddUnique(Linked->GetOwningNode());
                }
            }
            Result.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
            return Result;
        }

        // 一个执行流 = 从「没有执行输入的节点」出发能通过执行线到达的全部节点。
        // 祖先上还挂着选中节点时，把该流入口提前到祖先，保证一条链不会被切成两个区域。
        TArray<UEdGraphNode*> CollectFlowRoots(const TArray<UEdGraphNode*>& ExecNodes, const TSet<UEdGraphNode*>& Selected)
        {
            TArray<UEdGraphNode*> Roots;
            for (UEdGraphNode* Node : ExecNodes)
            {
                UEdGraphNode* Root = Node;
                TSet<UEdGraphNode*> Seen;
                bool bMoved = true;
                while (bMoved)
                {
                    bMoved = false;
                    for (UEdGraphNode* Parent : Neighbours(Root, EGPD_Input, true))
                    {
                        if (Seen.Contains(Parent))
                        {
                            continue;
                        }
                        Seen.Add(Parent);
                        if (!Selected.Contains(Parent))
                        {
                            continue;
                        }
                        if (!HasExecution(Parent))
                        {
                            continue;
                        }
                        Root = Parent;
                        bMoved = true;
                    }
                }
                Roots.AddUnique(Root);
            }
            Roots.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
            return Roots;
        }

        void ReachableFrom(UEdGraphNode* Root, TSet<UEdGraphNode*>& Out)
        {
            TArray<UEdGraphNode*> Stack{ Root };
            while (Stack.Num())
            {
                UEdGraphNode* Node = Stack.Pop();
                if (!Node || Out.Contains(Node))
                {
                    continue;
                }
                Out.Add(Node);
                for (UEdGraphNode* Child : Neighbours(Node, EGPD_Output, true))
                {
                    Stack.Add(Child);
                }
            }
        }

        // 数据节点的「深度」= 沿数据线向消费者方向的最短距离；消费者列号越大同行越靠右。
        int32 ComputeDataDepth(UEdGraphNode* Node, TMap<UEdGraphNode*, int32>& Cache, TSet<UEdGraphNode*>& Visiting)
        {
            if (const int32* Found = Cache.Find(Node))
            {
                return *Found;
            }
            if (Visiting.Contains(Node))
            {
                return 1;
            }
            Visiting.Add(Node);
            int32 Depth = 1;
            const TArray<UEdGraphNode*> Consumers = Neighbours(Node, EGPD_Output, false);
            if (Consumers.Num() > 0)
            {
                int32 MinConsumer = MAX_int32;
                for (UEdGraphNode* Consumer : Consumers)
                {
                    MinConsumer = FMath::Min(MinConsumer, ComputeDataDepth(Consumer, Cache, Visiting));
                }
                Depth = MinConsumer + 1;
            }
            Visiting.Remove(Node);
            Cache.Add(Node, Depth);
            return Depth;
        }

        // 跨流连线只报告不修改：自动断线会静默改坏逻辑。
        void ReportCrossFlowTies(const TArray<TArray<UEdGraphNode*>>& Flows,
                                 TArray<TSharedPtr<FJsonValue>>& OutWarnings)
        {
            TMap<UEdGraphNode*, int32> FlowOf;
            for (int32 Index = 0; Index < Flows.Num(); ++Index)
            {
                for (UEdGraphNode* Node : Flows[Index])
                {
                    FlowOf.Add(Node, Index);
                }
            }
            TArray<UEdGraphNode*> FlowNodes;
            FlowOf.GetKeys(FlowNodes);
            FlowNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
            for (UEdGraphNode* Node : FlowNodes)
            {
                const int32 Owner = FlowOf[Node];
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin->Direction != EGPD_Output || IsExecPin(Pin))
                    {
                        continue;
                    }
                    for (UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Target = Linked ? Linked->GetOwningNode() : nullptr;
                        const int32* TargetFlow = Target ? FlowOf.Find(Target) : nullptr;
                        // 数据节点会被多条流共用，只有两端都是执行节点时才算跨流
                        if (!TargetFlow || *TargetFlow == Owner || !HasExecution(Target) || !HasExecution(Node))
                        {
                            continue;
                        }
                        TSharedPtr<FJsonObject> Tie = MakeShared<FJsonObject>();
                        Tie->SetStringField(TEXT("kind"), TEXT("cross_flow_link"));
                        Tie->SetStringField(TEXT("detail"), FString::Printf(TEXT("%s.%s -> %s"),
                            *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                            *Pin->PinName.ToString(),
                            *Target->GetNodeTitle(ENodeTitleType::ListView).ToString()));
                        OutWarnings.Add(MakeShared<FJsonValueObject>(Tie));
                    }
                }
            }
        }
    }

    FVector2D Measure(UEdGraphNode* Node, bool& bEstimated)
    {
        bEstimated = true;
        if (FSlateApplication::IsInitialized())
        {
            TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Node);
            if (Widget.IsValid())
            {
                Widget->SlatePrepass(1.0f);
                const FVector2D Size = Widget->GetDesiredSize();
                if (Size.X > 0.0f && Size.Y > 0.0f)
                {
                    bEstimated = false;
                    return Size;
                }
            }
        }
        // 无可用 Slate 尺寸时保守估算，并在命令结果中逐个标注
        return FVector2D(320.0f, FMath::Max(128.0f, 64.0f + Node->Pins.Num() * 24.0f));
    }

    void PlaceNewNode(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params)
    {
        if (Params->HasField(TEXT("node_position")))
        {
            return;
        }
        bool bEstimated = false;
        const FVector2D Size = Measure(Node, bEstimated);
        FVector2D Position(0.0f, 0.0f);
        UEdGraph* Graph = Node->GetGraph();
        bool bMoved = true;
        while (bMoved)
        {
            bMoved = false;
            for (UEdGraphNode* Other : Graph->Nodes)
            {
                if (Other == Node || Other->IsA<UEdGraphNode_Comment>())
                {
                    continue;
                }
                if (Overlaps(Position, Size, Other))
                {
                    bool bOtherEstimated = false;
                    Position.Y = Snap(Other->NodePosY + Measure(Other, bOtherEstimated).Y + Gap);
                    bMoved = true;
                }
            }
        }
        Node->NodePosX = Snap(Position.X);
        Node->NodePosY = Snap(Position.Y);
    }

    TSharedPtr<FJsonObject> Describe(UEdGraph* Graph)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Nodes;
        TArray<TSharedPtr<FJsonValue>> Connections;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            bool bEstimated = false;
            const FVector2D Size = Measure(Node, bEstimated);
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
            Item->SetNumberField(TEXT("x"), Node->NodePosX);
            Item->SetNumberField(TEXT("y"), Node->NodePosY);
            Item->SetNumberField(TEXT("width"), Size.X);
            Item->SetNumberField(TEXT("height"), Size.Y);
            Item->SetStringField(TEXT("size_source"), bEstimated ? TEXT("estimate") : TEXT("slate_desired_size"));
            Nodes.Add(MakeShared<FJsonValueObject>(Item));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
                    Edge->SetStringField(TEXT("source_node_id"), Node->NodeGuid.ToString());
                    Edge->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
                    Edge->SetStringField(TEXT("target_node_id"), Linked->GetOwningNode()->NodeGuid.ToString());
                    Edge->SetStringField(TEXT("target_pin"), Linked->PinName.ToString());
                    Edge->SetBoolField(TEXT("execution"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
                    Connections.Add(MakeShared<FJsonValueObject>(Edge));
                }
            }
        }
        Result->SetArrayField(TEXT("nodes"), Nodes);
        Result->SetArrayField(TEXT("connections"), Connections);
        return Result;
    }

    TSharedPtr<FJsonObject> Format(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
    {
        bool bWholeGraph = false;
        Params->TryGetBoolField(TEXT("whole_graph"), bWholeGraph);
        bool bDryRun = false;
        Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
        // mode=unplaced 只整理从未被显式定位过的节点，避免冲掉手工布局
        FString Mode = TEXT("selection");
        Params->TryGetStringField(TEXT("mode"), Mode);
        const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
        const bool bHasIds = Params->TryGetArrayField(TEXT("node_ids"), Ids);

        TSet<UEdGraphNode*> Selected;
        if (Mode == TEXT("unplaced"))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && !Node->IsA<UEdGraphNode_Comment>() && Node->NodePosX == 0 && Node->NodePosY == 0)
                {
                    Selected.Add(Node);
                }
            }
            if (Selected.Num() == 0)
            {
                TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
                Empty->SetNumberField(TEXT("formatted_count"), 0);
                Empty->SetArrayField(TEXT("warnings"), {});
                return Empty;
            }
        }
        else if (bWholeGraph == bHasIds)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Choose node_ids or whole_graph=true"));
        }
        else if (bHasIds)
        {
            for (const TSharedPtr<FJsonValue>& Id : *Ids)
            {
                FString Value;
                if (!Id->TryGetString(Value))
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("node_ids must contain strings"));
                }
                UEdGraphNode* Found = nullptr;
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node->NodeGuid.ToString() == Value)
                    {
                        Found = Node;
                        break;
                    }
                }
                if (!Found)
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Unknown node_id in selection"));
                }
                if (Found->IsA<UEdGraphNode_Comment>())
                {
                    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Comment nodes cannot be selected for automatic layout"));
                }
                Selected.Add(Found);
            }
        }
        else
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node->IsA<UEdGraphNode_Comment>())
                {
                    Selected.Add(Node);
                }
            }
        }

        TArray<UEdGraphNode*> Ordered = Selected.Array();
        Ordered.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
        TMap<UEdGraphNode*, FVector2D> Sizes;
        for (UEdGraphNode* Node : Ordered)
        {
            bool bEstimated = false;
            Sizes.Add(Node, Measure(Node, bEstimated));
        }

        TArray<UEdGraphNode*> ExecNodes;
        for (UEdGraphNode* Node : Ordered)
        {
            if (HasExecution(Node))
            {
                ExecNodes.Add(Node);
            }
        }

        TArray<TSharedPtr<FJsonValue>> Warnings;
        TMap<UEdGraphNode*, FVector2D> Positions;
        TSet<UEdGraphNode*> Visiting;
        float CursorY = 0.0f;
        int32 FlowCount = 0;

        // 每条流单独占一条水平带：流内排完再累加 Y，流之间永不交错。
        const TArray<UEdGraphNode*> Roots = CollectFlowRoots(ExecNodes, Selected);
        TArray<TArray<UEdGraphNode*>> Flows;
        for (UEdGraphNode* Root : Roots)
        {
            TSet<UEdGraphNode*> Members;
            ReachableFrom(Root, Members);
            TArray<UEdGraphNode*> FlowNodes;
            for (UEdGraphNode* Member : Members)
            {
                if (Selected.Contains(Member))
                {
                    FlowNodes.Add(Member);
                }
            }
            // 只保留执行节点作为布局骨架，数据节点稍后按深度入列
            TArray<UEdGraphNode*> FlowExec;
            for (UEdGraphNode* Member : FlowNodes)
            {
                if (HasExecution(Member))
                {
                    FlowExec.Add(Member);
                }
            }
            if (FlowExec.Num() == 0)
            {
                continue;
            }
            ++FlowCount;
            const float FlowTop = CursorY;
            float FlowBottom = FlowTop;

            // 流内执行链：宽度优先按列推进，同一列共 X，分支各自占子行
            TMap<UEdGraphNode*, int32> ColumnOf;
            TMap<UEdGraphNode*, float> RowOf;
            TArray<float> ColumnWidths;
            TFunction<void(UEdGraphNode*, int32, float)> Walk;
            Walk = [&](UEdGraphNode* Node, int32 Column, float RowY)
            {
                if (!Selected.Contains(Node))
                {
                    return;
                }
                if (Visiting.Contains(Node))
                {
                    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Execution cycle retained; back edges may cross")));
                    return;
                }
                if (const int32* Existing = ColumnOf.Find(Node))
                {
                    if (*Existing == Column)
                    {
                        RowOf[Node] = FMath::Max(RowOf[Node], RowY);
                    }
                    else
                    {
                        Warnings.Add(MakeShared<FJsonValueString>(TEXT("Shared execution node retained; converging wires may cross")));
                    }
                    return;
                }
                ColumnOf.Add(Node, Column);
                RowOf.Add(Node, RowY);
                while (ColumnWidths.Num() <= Column)
                {
                    ColumnWidths.Add(0.0f);
                }
                ColumnWidths[Column] = FMath::Max(ColumnWidths[Column], Sizes[Node].X);
                Visiting.Add(Node);

                float BranchY = RowY;
                bool bFirst = true;
                for (UEdGraphNode* Child : Neighbours(Node, EGPD_Output, true))
                {
                    if (bFirst)
                    {
                        bFirst = false;
                        Walk(Child, Column + 1, RowY);
                    }
                    else
                    {
                        BranchY += FMath::Max(Sizes[Node].Y, 96.0f) + Gap;
                        Walk(Child, Column + 1, BranchY);
                    }
                }
                Visiting.Remove(Node);
            };

            // 流入口按 GUID 稳定排序，保证可重复
            TArray<UEdGraphNode*> FlowRoots;
            for (UEdGraphNode* Node : FlowExec)
            {
                bool bHasSelectedParent = false;
                for (UEdGraphNode* Parent : Neighbours(Node, EGPD_Input, true))
                {
                    if (Selected.Contains(Parent) && HasExecution(Parent))
                    {
                        bHasSelectedParent = true;
                        break;
                    }
                }
                if (!bHasSelectedParent)
                {
                    FlowRoots.Add(Node);
                }
            }
            FlowRoots.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid < B.NodeGuid; });
            float EntryY = FlowTop;
            for (UEdGraphNode* Root : FlowRoots)
            {
                Walk(Root, 0, EntryY);
                EntryY += FMath::Max(Sizes[Root].Y, 96.0f) + Gap;
            }
            // 没有入口的环同样按稳定顺序兜底
            for (UEdGraphNode* Node : FlowExec)
            {
                if (!ColumnOf.Contains(Node))
                {
                    Walk(Node, 0, EntryY);
                    EntryY += FMath::Max(Sizes[Node].Y, 96.0f) + Gap;
                }
            }

            // 列 X 由累计列宽决定，同列严格相等
            TArray<float> ColumnX;
            float X = 0.0f;
            for (int32 Column = 0; Column < ColumnWidths.Num(); ++Column)
            {
                ColumnX.Add(Snap(X));
                X += ColumnWidths[Column] + Gap;
            }
            for (const auto& Pair : ColumnOf)
            {
                const int32 Column = Pair.Value;
                const float Y = FlowTop + RowOf[Pair.Key];
                Positions.Add(Pair.Key, FVector2D(ColumnX[Column], Snap(Y)));
                FlowBottom = FMath::Max(FlowBottom, Snap(Y) + Sizes[Pair.Key].Y);
            }

            // 数据节点按深度分列，列内按消费者 Y 顺序排列，保证同深度共 X
            TMap<UEdGraphNode*, int32> DataDepth;
            TArray<UEdGraphNode*> DataNodes;
            for (UEdGraphNode* Node : FlowNodes)
            {
                if (!HasExecution(Node) && ColumnOf.Contains(Node) == false)
                {
                    DataNodes.Add(Node);
                }
            }
            // 消费者在本流内的数据节点才参与该流的数据列
            TArray<UEdGraphNode*> LocalData;
            for (UEdGraphNode* Node : DataNodes)
            {
                for (UEdGraphNode* Consumer : Neighbours(Node, EGPD_Output, false))
                {
                    if (ColumnOf.Contains(Consumer))
                    {
                        LocalData.Add(Node);
                        break;
                    }
                }
            }
            TMap<int32, TArray<UEdGraphNode*>> DataByDepth;
            for (UEdGraphNode* Node : LocalData)
            {
                TSet<UEdGraphNode*> DepthVisiting;
                const int32 Depth = ComputeDataDepth(Node, DataDepth, DepthVisiting);
                DataByDepth.FindOrAdd(Depth).Add(Node);
            }
            TArray<int32> Depths;
            DataByDepth.GetKeys(Depths);
            Depths.Sort();
            for (int32 Depth : Depths)
            {
                TArray<UEdGraphNode*>& Column = DataByDepth[Depth];
                Column.Sort([&](UEdGraphNode& A, UEdGraphNode& B)
                {
                    return Positions.Contains(&A) ? false : Positions.Contains(&B) ? true : A.NodeGuid < B.NodeGuid;
                });
                const float ColumnLeft = FMath::Max(0.0f,
                    Snap(X) - DataColumnStep * Depth);
                // 数据列贴着本流已排好的执行节点纵向分布，避免溢到相邻流的水平带
                // 每个数据节点贴住自己消费者的 Y，但不早于流顶；同列按顺序向下堆叠
                float Y = FlowTop;
                for (UEdGraphNode* Node : Column)
                {
                    float Anchor = Y;
                    for (UEdGraphNode* Consumer : Neighbours(Node, EGPD_Output, false))
                    {
                        if (const FVector2D* ConsumerPos = Positions.Find(Consumer))
                        {
                            Anchor = FMath::Max(Y, FMath::Max(FlowTop, ConsumerPos->Y));
                            break;
                        }
                    }
                    Positions.Add(Node, FVector2D(ColumnLeft, Snap(Anchor)));
                    FlowBottom = FMath::Max(FlowBottom, Snap(Anchor) + Sizes[Node].Y);
                    Y = Snap(Anchor) + Sizes[Node].Y + Gap;
                }
            }

            CursorY = Snap(FlowBottom + FlowSeperation);
            for (UEdGraphNode* Node : FlowNodes)
            {
                if (HasExecution(Node))
                {
                    Flows.Add(FlowExec);
                    break;
                }
            }
        }

        // 未被任何执行流覆盖的选中节点（孤立数据节点）按稳定顺序单独排列
        float OrphanY = CursorY;
        for (UEdGraphNode* Node : Ordered)
        {
            if (Positions.Contains(Node))
            {
                continue;
            }
            Positions.Add(Node, FVector2D(0.0f, Snap(OrphanY)));
            OrphanY = Snap(OrphanY + Sizes[Node].Y + Gap);
        }

        // 注释框是纯手工意图：既不当容器也不被移动，但必须当实心障碍
        TArray<UEdGraphNode*> Comments;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            // 注释框从不当容器也不被移动，但必须当实心障碍让节点绕开
            if (Node && Node->IsA<UEdGraphNode_Comment>())
            {
                Comments.Add(Node);
            }
        }
        for (auto& Pair : Positions)
        {
            FVector2D Position = Pair.Value;
            bool bMoved = true;
            int32 Guard = 0;
            while (bMoved && Guard++ < 512)
            {
                bMoved = false;
                for (UEdGraphNode* Comment : Comments)
                {
                    if (Overlaps(Position, Sizes[Pair.Key], Comment))
                    {
                        bool bEstimated = false;
                        Position.Y = Snap(Comment->NodePosY + Measure(Comment, bEstimated).Y + Gap);
                        bMoved = true;
                    }
                }
            }
            Pair.Value = Position;
        }

        // 只报告跨流连线，绝不自动断开
        {
            TArray<TArray<UEdGraphNode*>> FlowMembers;
            for (UEdGraphNode* Root : Roots)
            {
                TSet<UEdGraphNode*> Members;
                ReachableFrom(Root, Members);
                TArray<UEdGraphNode*> Kept;
                for (UEdGraphNode* Member : Members)
                {
                    if (Selected.Contains(Member))
                    {
                        Kept.Add(Member);
                    }
                }
                if (Kept.Num())
                {
                    FlowMembers.Add(Kept);
                }
            }
            ReportCrossFlowTies(FlowMembers, Warnings);
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Planned;
        for (const auto& Pair : Positions)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), Pair.Key->NodeGuid.ToString());
            Item->SetStringField(TEXT("title"), Pair.Key->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetNumberField(TEXT("x"), Snap(Pair.Value.X));
            Item->SetNumberField(TEXT("y"), Snap(Pair.Value.Y));
            Planned.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("planned"), Planned);
        Result->SetNumberField(TEXT("formatted_count"), Positions.Num());
        Result->SetNumberField(TEXT("flow_count"), FlowCount);
        Result->SetArrayField(TEXT("warnings"), Warnings)
        ;
        Result->SetBoolField(TEXT("dry_run"), bDryRun);
        if (bDryRun)
        {
            return Result;
        }

        Graph->Modify();
        for (const auto& Pair : Positions)
        {
            Pair.Key->SetFlags(RF_Transactional);
            Pair.Key->Modify();
            Pair.Key->NodePosX = Snap(Pair.Value.X);
            Pair.Key->NodePosY = Snap(Pair.Value.Y);
        }
        Graph->NotifyGraphChanged();
        TSharedPtr<FJsonObject> Final = Describe(Graph);
        Final->SetNumberField(TEXT("formatted_count"), Positions.Num());
        Final->SetNumberField(TEXT("flow_count"), FlowCount);
        Final->SetArrayField(TEXT("warnings"), Warnings);
        Final->SetBoolField(TEXT("dry_run"), false);
        return Final;
    }
}
