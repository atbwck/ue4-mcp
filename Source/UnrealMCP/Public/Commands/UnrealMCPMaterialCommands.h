#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class AActor;
class UMaterial;
class UMaterialExpression;
class UMaterialInterface;
class UMeshComponent;

/**
 * 处理材质相关命令：材质资产图编辑、材质实例参数编辑，以及网格组件的材质分配。
 *
 * 寻址规则：材质图节点用 node_id 标识，优先取表达式的 MaterialExpressionGuid；
 * 表达式没有有效 GUID 时回落到对象名。两者都在 get_material_graph 里返回。
 */
class UNREALMCP_API FUnrealMCPMaterialCommands
{
public:
    FUnrealMCPMaterialCommands();

    // Handle material commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // 按名字在指定世界里找 Actor；PIE 场景下搜 PIE 世界，否则搜编辑器世界。
    static AActor* FindWorldActor(const FString& ActorName, const TSharedPtr<FJsonObject>& Params);
    // 按名字找网格组件。
    static UMeshComponent* FindMeshComponent(AActor* Actor, const FString& ComponentName);

    // Specific material command handlers
    TSharedPtr<FJsonObject> HandleCreateMaterial(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddVectorParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialOnComponent(const TSharedPtr<FJsonObject>& Params);
    // 为指定槽位创建动态材质实例并赋给组件（仅 PIE/运行时有效，编辑器世界不持久化）。
    TSharedPtr<FJsonObject> HandleCreateDynamicMaterialInstance(const TSharedPtr<FJsonObject>& Params);
    // 设置动态材质实例的标量/向量参数。
    TSharedPtr<FJsonObject> HandleSetMaterialParameter(const TSharedPtr<FJsonObject>& Params);
    // 报告组件每个材质槽的资产路径、是否 DMI、父材质与参数值，用于验证赋值结果。
    TSharedPtr<FJsonObject> HandleGetComponentMaterials(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> HandleGetMaterialGraph(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListMaterialParameters(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddMaterialExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialExpressionProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleConnectMaterialProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDisconnectMaterialProperty(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialSettings(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddMaterialComment(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFormatMaterialGraph(const TSharedPtr<FJsonObject>& Params);

    TSharedPtr<FJsonObject> HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetMaterialInstanceParameters(const TSharedPtr<FJsonObject>& Params);
};
