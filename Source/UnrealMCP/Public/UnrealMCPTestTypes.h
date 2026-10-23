// 回归测试专用的最小反射类型：不依赖项目模块，供属性写入分支测试使用。

#pragma once

#include "CoreMinimal.h"
#include "UnrealMCPTestTypes.generated.h"

// 含 TSet<FName> 的测试载体，验证 SetObjectProperty 的 TSet 分支。
UCLASS()
class UMCPTestSetObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	TSet<FName> NameSet;
};
