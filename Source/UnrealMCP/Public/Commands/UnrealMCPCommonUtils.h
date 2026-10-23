#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "EdGraph/EdGraphNode.h"

// Forward declarations
class AActor;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node_Event;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_InputAction;
class UK2Node_Self;
class UFunction;

/**
 * Common utilities for UnrealMCP commands
 */
class UNREALMCP_API FUnrealMCPCommonUtils
{
public:
    // JSON utilities
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& Message);
    static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data = nullptr);
    static void GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray);
    static void GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray);
    static FVector2D GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FVector GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FRotator GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

    // Actor utilities
    static TSharedPtr<FJsonValue> ActorToJson(AActor* Actor);
    static TSharedPtr<FJsonObject> ActorToJsonObject(AActor* Actor, bool bDetailed = false);

    // 创建资产前验证包名和对象名，避免外部输入触发引擎断言。
    static bool ValidateAssetCreationPath(const FString& PackagePath, const FString& AssetName, FString& OutError);

    // Blueprint utilities
    static UBlueprint* FindBlueprint(const FString& BlueprintName);
    static UBlueprint* FindBlueprintByName(const FString& BlueprintName);
    static UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);
    // 图作用域：缺省回落到第一个 Event Graph（旧行为），指定 graph_name 时可解析
    // ubergraph、函数图和宏图；解析失败时 OutError 说明候选集合。
    static UEdGraph* ResolveGraphForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError,
                                            bool bAllowCreate = true);
    static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

    // Blueprint node utilities
    static UK2Node_Event* CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position);
    static UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position);
    static UK2Node_VariableGet* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_VariableSet* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_InputAction* CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position);
    static UK2Node_Self* CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position);
    static bool ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                UEdGraphNode* TargetNode, const FString& TargetPinName);
    static bool DisconnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName,
                                UEdGraphNode* TargetNode, const FString& TargetPinName);
    static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeId);
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static UK2Node_Event* FindExistingEventNode(UEdGraph* Graph, const FString& EventName);

    // Property utilities
    // Object 支持任意 UObject 或 UScriptStruct 实例指针（如 TSet 属性测试用的结构体）。
    static bool SetObjectProperty(void* Object, const FString& PropertyName,
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);

    // 可靠保存：先走 UEditorAssetLibrary，AssetRegistry 未就绪等场景回退 UPackage::SavePackage 直存
    static bool SaveLoadedAssetReliable(UObject* Asset, FString& OutError);
    // 编译蓝图并返回错误/警告的数量与逐条文本。
    // 返回 false 时 OutError 是可直接回给客户端的汇总信息。
    static bool CompileBlueprintWithDiagnostics(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutDiagnostics, FString& OutError);
    // 写命令默认落盘；客户端传 defer_save=true 时跳过，由 save_assets 统一提交。
    // deferred=true 时返回成功但不保存，响应里需要报告 deferred_save 以便调用方知道尚未落盘。
    static bool ShouldDeferSave(const TSharedPtr<FJsonObject>& Params);
    static bool SaveUnlessDeferred(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FString& OutError);
};
