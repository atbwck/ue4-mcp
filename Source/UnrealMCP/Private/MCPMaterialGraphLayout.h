#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UMaterial;

// 材质图结构语义：只调整表达式的编辑器坐标，不改节点、连接和材质属性
namespace MCPMaterialGraphLayout
{
    TSharedPtr<FJsonObject> Describe(UMaterial* Material);
    TSharedPtr<FJsonObject> Format(UMaterial* Material, const TSharedPtr<FJsonObject>& Params);
}
