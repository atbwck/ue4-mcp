#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Project-wide MCP commands
 */
class UNREALMCP_API FUnrealMCPProjectCommands
{
public:
    FUnrealMCPProjectCommands();

    // Handle project commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Specific project command handlers
    TSharedPtr<FJsonObject> HandleCreateInputMapping(const TSharedPtr<FJsonObject>& Params);

    // DataTable commands (V2 config basis)
    TSharedPtr<FJsonObject> HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAddRow(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReadRow(const TSharedPtr<FJsonObject>& Params);

    // 按资产路径设置任意 UObject（如 DataAsset 实例）的属性并保存
    TSharedPtr<FJsonObject> HandleSetAssetProperty(const TSharedPtr<FJsonObject>& Params);

    // 按当前已加载的 C++ 反射刷新蓝图节点；不能生成或重载原生反射。
    TSharedPtr<FJsonObject> HandleRefreshProjectReflection(const TSharedPtr<FJsonObject>& Params);
    // 触发可选的 Live Coding 控制台命令；只报告请求接受，不报告编译完成。
    TSharedPtr<FJsonObject> HandleCompileProject(const TSharedPtr<FJsonObject>& Params);
    // 显式提交延迟保存的资产（配合各命令的 defer_save=true）
    TSharedPtr<FJsonObject> HandleSaveAssets(const TSharedPtr<FJsonObject>& Params);
};
