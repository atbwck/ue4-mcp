#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UEdGraph;
class UEdGraphNode;

// 图表排版仅改变坐标；所有连接与节点身份由原图保留
namespace MCPGraphLayout
{
    FVector2D Measure(UEdGraphNode* Node, bool& bEstimated);
    void PlaceNewNode(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> Describe(UEdGraph* Graph);
    TSharedPtr<FJsonObject> Format(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params);
}
