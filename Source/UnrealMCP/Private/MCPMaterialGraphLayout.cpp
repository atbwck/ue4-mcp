#include "MCPMaterialGraphLayout.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "SceneTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MCPMaterialGraphLayout
{
    // 与蓝图图排版共用同一套节奏：16 网格、列间 64、每个属性根一行的纵向偏移
    const int32 Grid = 16;
    const int32 ColumnGap = 64;

    int32 Snap(int32 Value)
    {
        return FMath::RoundToInt((float)Value / (float)Grid) * Grid;
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

    bool IsComment(UMaterialExpression* Expression)
    {
        return Expression && Expression->IsA<UMaterialExpressionComment>();
    }

    // 材质属性输入是整张图的根：从属性出发，数据线只向左侧展开。
    struct FInputAnchor
    {
        EMaterialProperty Property;
        FExpressionInput* Input;
        UMaterialExpression* Expression;
    };

    TArray<FInputAnchor> CollectAnchors(UMaterial* Material)
    {
        TArray<FInputAnchor> Anchors;
        for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
        {
            const EMaterialProperty Property = static_cast<EMaterialProperty>(PropertyIndex);
            FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
            if (Input && Input->Expression)
            {
                Anchors.Add({Property, Input, Input->Expression});
            }
        }
        return Anchors;
    }

    FString PropertyName(EMaterialProperty Property)
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

    TSharedPtr<FJsonObject> Describe(UMaterial* Material)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("material_path"), Material->GetPathName());

        TArray<TSharedPtr<FJsonValue>> Expressions;
        for (UMaterialExpression* Expression : Material->Expressions)
        {
            if (!Expression)
            {
                continue;
            }
            TArray<FString> Captions;
            Expression->GetCaption(Captions);

            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), NodeIdOf(Expression));
            Item->SetStringField(TEXT("name"), Expression->GetName());
            Item->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
            Item->SetStringField(TEXT("caption"), FString::Join(Captions, TEXT(" ")));
            Item->SetStringField(TEXT("desc"), Expression->Desc);
            Item->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
            Item->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
            Item->SetNumberField(TEXT("width"), Expression->GetWidth());
            Item->SetNumberField(TEXT("height"), Expression->GetHeight());

            if (Expression->HasAParameterName()) Item->SetStringField(TEXT("parameter_name"), Expression->GetParameterName().ToString());
            if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression))
            {
                Item->SetStringField(TEXT("parameter_name"), Parameter->ParameterName.ToString());
                Item->SetStringField(TEXT("parameter_group"), Parameter->Group.ToString());
                Item->SetNumberField(TEXT("sort_priority"), Parameter->SortPriority);
            }

            // 未命名的输入（第一个输入）不带名字，用空字符串表示，调用方按索引引用
            TArray<FExpressionInput*> Inputs = Expression->GetInputs();
            TArray<TSharedPtr<FJsonValue>> InputItems;
            for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
            {
                TSharedPtr<FJsonObject> InputItem = MakeShared<FJsonObject>();
                InputItem->SetNumberField(TEXT("output_index"), Inputs[InputIndex] ? Inputs[InputIndex]->OutputIndex : 0);
                InputItem->SetNumberField(TEXT("index"), InputIndex);
                const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
                InputItem->SetStringField(TEXT("name"), FunctionCall
                    ? FunctionCall->GetInputNameWithType(InputIndex, false).ToString()
                    : Expression->GetInputName(InputIndex).ToString());
                InputItem->SetNumberField(TEXT("output_index"), Inputs[InputIndex] ? Inputs[InputIndex]->OutputIndex : 0);
                InputItem->SetStringField(TEXT("connected_node_id"),
                    Inputs[InputIndex] && Inputs[InputIndex]->Expression ? NodeIdOf(Inputs[InputIndex]->Expression) : FString());
                InputItems.Add(MakeShared<FJsonValueObject>(InputItem));
            }
            Item->SetArrayField(TEXT("inputs"), InputItems);

            TArray<TSharedPtr<FJsonValue>> OutputItems;
            const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
            for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
            {
                TSharedPtr<FJsonObject> OutputItem = MakeShared<FJsonObject>();
                OutputItem->SetNumberField(TEXT("index"), OutputIndex);
                OutputItem->SetStringField(TEXT("name"), Outputs[OutputIndex].OutputName.ToString());
                OutputItems.Add(MakeShared<FJsonValueObject>(OutputItem));
            }
            Item->SetArrayField(TEXT("outputs"), OutputItems);
            Expressions.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("expressions"), Expressions);

        TArray<TSharedPtr<FJsonValue>> Properties;
        for (const FInputAnchor& Anchor : CollectAnchors(Material))
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("property"), PropertyName(Anchor.Property));
            Item->SetStringField(TEXT("source_node_id"), NodeIdOf(Anchor.Expression));
            Item->SetNumberField(TEXT("output_index"), Anchor.Input->OutputIndex);
            Properties.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("property_inputs"), Properties);

        TArray<TSharedPtr<FJsonValue>> Comments;
        for (UMaterialExpressionComment* Comment : Material->EditorComments)
        {
            if (!Comment)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), NodeIdOf(Comment));
            Item->SetStringField(TEXT("name"), Comment->GetName());
            Item->SetStringField(TEXT("text"), Comment->Text);
            Item->SetNumberField(TEXT("x"), Comment->MaterialExpressionEditorX);
            Item->SetNumberField(TEXT("y"), Comment->MaterialExpressionEditorY);
            Item->SetNumberField(TEXT("size_x"), Comment->SizeX);
            Item->SetNumberField(TEXT("size_y"), Comment->SizeY);
            Comments.Add(MakeShared<FJsonValueObject>(Item));
        }
        Result->SetArrayField(TEXT("comments"), Comments);

        const UEnum* BlendEnum = StaticEnum<EBlendMode>();
        const UEnum* ShadingEnum = StaticEnum<EMaterialShadingModel>();
        const UEnum* DomainEnum = StaticEnum<EMaterialDomain>();
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(TEXT("blend_mode"),
            BlendEnum ? BlendEnum->GetNameStringByValue(static_cast<int64>(Material->BlendMode.GetValue())) : FString());
        // ShadingModel 是私有字段：汇编后的可用性靠 GetShadingModels() 暴露，
        // 这里取第一个启用的模型名，与编辑器详情面板显示的一致。
        FString ShadingModelName;
        const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
        for (int32 Index = 0; Index < MSM_NUM; ++Index)
        {
            if (ShadingModels.HasShadingModel(static_cast<EMaterialShadingModel>(Index)))
            {
                ShadingModelName = ShadingEnum ? ShadingEnum->GetNameStringByValue(Index) : FString::FromInt(Index);
                break;
            }
        }
        Settings->SetStringField(TEXT("shading_model"), ShadingModelName);
        Settings->SetStringField(TEXT("material_domain"),
            DomainEnum ? DomainEnum->GetNameStringByValue(static_cast<int64>(Material->MaterialDomain.GetValue())) : FString());
        Settings->SetBoolField(TEXT("two_sided"), Material->TwoSided != 0);
        Settings->SetNumberField(TEXT("opacity_mask_clip_value"), Material->OpacityMaskClipValue);
        Result->SetObjectField(TEXT("settings"), Settings);
        return Result;
    }

    // 同一列节点共用 X：列宽取该列最宽的节点，X 用累计宽度而不是固定步长，
    // 这样宽节点不会压到右侧下一列。
    struct FLayoutPlan
    {
        TMap<UMaterialExpression*, int32> Column;
        TMap<UMaterialExpression*, int32> Row;
        TArray<int32> ColumnX;
        TArray<int32> ColumnWidth;
        int32 RowPitch = 0;
        TArray<TSharedPtr<FJsonValue>> Warnings;
    };

    void AssignDepth(UMaterialExpression* Expression, int32 Depth,
                     TMap<UMaterialExpression*, int32>& DepthOf, TSet<UMaterialExpression*>& Visiting)
    {
        if (!Expression || IsComment(Expression) || Visiting.Contains(Expression))
        {
            return;
        }
        // 深度取「离最近消费者」的最短距离，共享节点不会因为某条长路径被推到很左边
        const int32* Existing = DepthOf.Find(Expression);
        if (Existing && *Existing <= Depth)
        {
            return;
        }
        DepthOf.Add(Expression, Depth);
        Visiting.Add(Expression);
        for (FExpressionInput* Input : Expression->GetInputs())
        {
            if (Input && Input->Expression)
            {
                AssignDepth(Input->Expression, Depth + 1, DepthOf, Visiting);
            }
        }
        Visiting.Remove(Expression);
    }

    FLayoutPlan BuildPlan(UMaterial* Material)
    {
        FLayoutPlan Plan;
        TMap<UMaterialExpression*, int32> DepthOf;
        TSet<UMaterialExpression*> Visiting;

        // 材质属性一排就是最右列（第 0 列），数据深度决定左侧列号
        TArray<FInputAnchor> Anchors = CollectAnchors(Material);
        for (const FInputAnchor& Anchor : Anchors)
        {
            AssignDepth(Anchor.Expression, 0, DepthOf, Visiting);
        }

        // 没有接到任何材质属性的表达式（半成品节点）排在最后一列的左侧，保持稳定顺序
        TArray<UMaterialExpression*> Orphans;
        for (UMaterialExpression* Expression : Material->Expressions)
        {
            if (Expression && !IsComment(Expression) && !DepthOf.Contains(Expression))
            {
                Orphans.Add(Expression);
            }
        }
        Orphans.Sort([](const UMaterialExpression& A, const UMaterialExpression& B)
        {
            return NodeIdOf(const_cast<UMaterialExpression*>(&A)) < NodeIdOf(const_cast<UMaterialExpression*>(&B));
        });
        int32 MaxDepth = 0;
        for (const auto& Pair : DepthOf)
        {
            MaxDepth = FMath::Max(MaxDepth, Pair.Value);
        }
        for (UMaterialExpression* Orphan : Orphans)
        {
            DepthOf.Add(Orphan, MaxDepth + 1);
        }

        int32 NumColumns = MaxDepth + 2;
        for (const auto& Pair : DepthOf)
        {
            NumColumns = FMath::Max(NumColumns, Pair.Value + 1);
        }

        // 属性顺序决定纵向分组：BaseColor 的一组在上，Roughness 的一组在下
        Plan.ColumnWidth.Init(0, NumColumns);
        Plan.RowPitch = 0;
        for (const auto& Pair : DepthOf)
        {
            const int32 Column = NumColumns - 1 - Pair.Value;
            Plan.Column.Add(Pair.Key, Column);
            Plan.ColumnWidth[Column] = FMath::Max(Plan.ColumnWidth[Column], Pair.Key->GetWidth());
            Plan.RowPitch = FMath::Max(Plan.RowPitch, Pair.Key->GetHeight() + Grid);
        }
        Plan.RowPitch = FMath::Max(Plan.RowPitch, 128);

        Plan.ColumnX.Init(0, NumColumns);
        int32 X = 0;
        for (int32 Column = 0; Column < NumColumns; ++Column)
        {
            Plan.ColumnX[Column] = Snap(X);
            X += Plan.ColumnWidth[Column] + ColumnGap;
        }

        // 行分配：每条属性链从同一个行游标向下铺，兄弟节点在列内依次下移。
        // 游标按列记录，同列节点不会重叠；不同列各自独立向下堆叠。
        TMap<int32, int32> NextRowInColumn;
        int32 Row = 0;
        TFunction<void(UMaterialExpression*, int32)> PlaceChain;
        PlaceChain = [&](UMaterialExpression* Expression, int32 PreferredRow)
        {
            if (!Expression || IsComment(Expression))
            {
                return;
            }
            const int32* ColumnPtr = Plan.Column.Find(Expression);
            if (!ColumnPtr)
            {
                return;
            }
            if (Plan.Row.Contains(Expression))
            {
                return;
            }
            const int32 Column = *ColumnPtr;
            int32& ColumnCursor = NextRowInColumn.FindOrAdd(Column);
            const int32 AssignedRow = FMath::Max(PreferredRow, ColumnCursor);
            Plan.Row.Add(Expression, AssignedRow);
            ColumnCursor = AssignedRow + 1;

            for (FExpressionInput* Input : Expression->GetInputs())
            {
                if (Input && Input->Expression)
                {
                    PlaceChain(Input->Expression, AssignedRow);
                }
            }
        };

        for (const FInputAnchor& Anchor : Anchors)
        {
            PlaceChain(Anchor.Expression, Row);
            Row += 1;
        }
        for (UMaterialExpression* Orphan : Orphans)
        {
            PlaceChain(Orphan, Row);
            Row += 1;
        }
        // 兜底：任何还没排上的表达式放到末尾，不静默丢节点
        for (const auto& Pair : Plan.Column)
        {
            if (!Plan.Row.Contains(Pair.Key))
            {
                PlaceChain(Pair.Key, Row);
                Row += 1;
            }
        }
        return Plan;
    }

    TSharedPtr<FJsonObject> Format(UMaterial* Material, const TSharedPtr<FJsonObject>& Params)
    {
        bool bDryRun = false;
        if (Params)
        {
            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
        }
        const FLayoutPlan Plan = BuildPlan(Material);
        const int32 RowPitch = Plan.RowPitch;

        TArray<TSharedPtr<FJsonValue>> Planned;
        for (const auto& Pair : Plan.Row)
        {
            UMaterialExpression* Expression = Pair.Key;
            const int32 Column = Plan.Column[Expression];
            const int32 X = Plan.ColumnX[Column];
            const int32 Y = Snap(Pair.Value * RowPitch);

            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), NodeIdOf(Expression));
            Item->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
            Item->SetNumberField(TEXT("x"), X);
            Item->SetNumberField(TEXT("y"), Y);
            Planned.Add(MakeShared<FJsonValueObject>(Item));

            if (!bDryRun)
            {
                Expression->Modify();
                Expression->MaterialExpressionEditorX = X;
                Expression->MaterialExpressionEditorY = Y;
            }
        }

        // 注释框是手工意图：默认只统计不移动，避免冲掉作者的分组
        TArray<TSharedPtr<FJsonValue>> Comments;
        for (UMaterialExpressionComment* Comment : Material->EditorComments)
        {
            if (!Comment)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("node_id"), NodeIdOf(Comment));
            Item->SetNumberField(TEXT("x"), Comment->MaterialExpressionEditorX);
            Item->SetNumberField(TEXT("y"), Comment->MaterialExpressionEditorY);
            Comments.Add(MakeShared<FJsonValueObject>(Item));
        }
        if (!bDryRun)
        {
            Material->PostEditChange();
            Material->MarkPackageDirty();
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("material_path"), Material->GetPathName());
        Result->SetArrayField(TEXT("planned"), Planned);
        Result->SetArrayField(TEXT("comments"), Comments);
        Result->SetArrayField(TEXT("warnings"), Plan.Warnings);
        Result->SetNumberField(TEXT("formatted_count"), Planned.Num());
        Result->SetNumberField(TEXT("column_count"), Plan.ColumnX.Num());
        Result->SetBoolField(TEXT("dry_run"), bDryRun);
        return Result;
    }
}
