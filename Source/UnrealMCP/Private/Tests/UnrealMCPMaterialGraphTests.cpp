#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Commands/UnrealMCPMaterialCommands.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialInstanceConstant.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "StaticParameterSet.h"
#include "AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"

namespace UnrealMCPMaterialTestHelpers
{
    const TCHAR* TestPath = TEXT("/Game/UnrealMCPMaterialTests");

    // 测试资产每次都新建，和会话里已有资产撞名只会让断言失真。
    FString MakeAssetPath(const FString& Prefix, FString& OutName)
    {
        OutName = Prefix + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
        return FString(TestPath) + TEXT("/") + OutName;
    }

    // 与命令实现同一个寻址规则：优先表达式 GUID，没有 GUID 时用对象名。
    FString NodeIdString(UMaterialExpression* Expression)
    {
        if (!Expression)
        {
            return FString();
        }
        FGuid& Guid = Expression->GetMaterialExpressionId();
        return Guid.IsValid() ? Guid.ToString() : Expression->GetName();
    }

    void DeleteTestAsset(const FString& AssetPath)
    {
        if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
        {
            UEditorAssetLibrary::DeleteAsset(AssetPath);
        }
    }

    TSharedPtr<FJsonObject> BaseParams(const FString& MaterialPath)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("material_path"), MaterialPath);
        Params->SetBoolField(TEXT("defer_save"), true);
        return Params;
    }

    // 命令处理器返回 {"success":..., "data":{...}}，桥接层再包成 {"status":..., "result":{...}}。
    // 两层都拆，测试才能同时接受直接调用和端到端调用。
    TSharedPtr<FJsonObject> Payload(const TSharedPtr<FJsonObject>& Response)
    {
        TSharedPtr<FJsonObject> Current = Response;
        for (int32 Depth = 0; Depth < 2 && Current.IsValid(); ++Depth)
        {
            const TSharedPtr<FJsonObject>* Inner = nullptr;
            if (Current->TryGetObjectField(TEXT("data"), Inner) && Inner && Inner->IsValid())
            {
                Current = *Inner;
                continue;
            }
            if (Current->TryGetObjectField(TEXT("result"), Inner) && Inner && Inner->IsValid())
            {
                Current = *Inner;
                continue;
            }
            break;
        }
        return Current;
    }

    bool IsError(const TSharedPtr<FJsonObject>& Response)
    {
        if (!Response.IsValid())
        {
            return true;
        }
        FString Status;
        if (Response->TryGetStringField(TEXT("status"), Status) && Status == TEXT("error"))
        {
            return true;
        }
        bool bSuccess = true;
        if (Response->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
        {
            return true;
        }
        return Response->HasField(TEXT("error"));
    }

    FString ErrorText(const TSharedPtr<FJsonObject>& Response)
    {
        if (!Response.IsValid())
        {
            return TEXT("null response");
        }
        FString Text;
        Response->TryGetStringField(TEXT("error"), Text);
        if (Text.IsEmpty())
        {
            Text = TEXT("unknown error");
        }
        return Text;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPMaterialGraphEditingTest,
    "UnrealMCP.Materials.GraphEditingRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPMaterialGraphEditingTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPMaterialTestHelpers;

    FString MaterialName;
    const FString MaterialPath = MakeAssetPath(TEXT("MCP_MaterialGraph"), MaterialName);
    DeleteTestAsset(MaterialPath);
    ON_SCOPE_EXIT { DeleteTestAsset(MaterialPath); };

    FUnrealMCPMaterialCommands Commands;

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("material_name"), MaterialName);
    CreateParams->SetStringField(TEXT("save_path"), TestPath);
    CreateParams->SetBoolField(TEXT("defer_save"), true);
    TSharedPtr<FJsonObject> CreateResult = Commands.HandleCommand(TEXT("create_material"), CreateParams);
    if (!TestFalse(TEXT("create_material succeeded"), IsError(CreateResult)))
    {
        AddError(ErrorText(CreateResult));
        return false;
    }

    UMaterial* Material = Cast<UMaterial>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!TestNotNull(TEXT("material loads back after creation"), Material))
    {
        return false;
    }

    // 无效属性必须在提交前拒绝，不能把半成品表达式留在材质资产里。
    TSharedPtr<FJsonObject> InvalidAddParams = BaseParams(MaterialPath);
    InvalidAddParams->SetStringField(TEXT("expression_class"), TEXT("Constant3Vector"));
    TSharedPtr<FJsonObject> InvalidProperties = MakeShared<FJsonObject>();
    InvalidProperties->SetStringField(TEXT("Constant"), TEXT("not-a-colour"));
    InvalidAddParams->SetObjectField(TEXT("properties"), InvalidProperties);
    TestTrue(TEXT("invalid expression property is rejected"),
        IsError(Commands.HandleCommand(TEXT("add_material_expression"), InvalidAddParams)));
    TestEqual(TEXT("invalid expression was not left in the graph"), Material->Expressions.Num(), 0);

    TArray<TPair<FString, double>> InvalidNumbers;
    InvalidNumbers.Emplace(TEXT("DefaultValue"), 1e100);
    InvalidNumbers.Emplace(TEXT("SortPriority"), 1.5);
    InvalidNumbers.Emplace(TEXT("SortPriority"), 1e100);
    for (const auto& Invalid : InvalidNumbers)
    {
        auto BadAdd = BaseParams(MaterialPath);
        BadAdd->SetStringField(TEXT("expression_class"), TEXT("ScalarParameter"));
        auto BadProperties = MakeShared<FJsonObject>();
        BadProperties->SetNumberField(Invalid.Key, Invalid.Value);
        BadAdd->SetObjectField(TEXT("properties"), BadProperties);
        TestTrue(TEXT("invalid numeric expression property is rejected"), IsError(Commands.HandleCommand(TEXT("add_material_expression"), BadAdd)));
        TestEqual(TEXT("invalid numeric expression leaves no node"), Material->Expressions.Num(), 0);
    }
    auto BadEnumAdd = BaseParams(MaterialPath);
    BadEnumAdd->SetStringField(TEXT("expression_class"), TEXT("TextureSample"));
    auto BadEnumProperties = MakeShared<FJsonObject>();
    BadEnumProperties->SetNumberField(TEXT("SamplerType"), 999);
    BadEnumAdd->SetObjectField(TEXT("properties"), BadEnumProperties);
    TestTrue(TEXT("invalid enum expression property is rejected"), IsError(Commands.HandleCommand(TEXT("add_material_expression"), BadEnumAdd)));
    TestEqual(TEXT("invalid enum expression leaves no node"), Material->Expressions.Num(), 0);
    for (const FString& InvalidText : TArray<FString>{TEXT("+"), TEXT("-"), TEXT("."), TEXT("")})
    {
        BadEnumProperties->SetStringField(TEXT("SamplerType"), InvalidText);
        TestTrue(TEXT("digitless enum numeric string is rejected"), IsError(Commands.HandleCommand(TEXT("add_material_expression"), BadEnumAdd)));
    }
    BadEnumAdd->SetStringField(TEXT("expression_class"), TEXT("BlendMaterialAttributes"));
    BadEnumProperties->Values.Empty();
    BadEnumProperties->SetStringField(TEXT("PixelAttributeBlendType"), FString::ChrN(NAME_SIZE - 1, TEXT('x')));
    TestTrue(TEXT("oversized namespaced enum is rejected without constructing an FName"), IsError(Commands.HandleCommand(TEXT("add_material_expression"), BadEnumAdd)));
    TestEqual(TEXT("invalid enum strings leave no nodes"), Material->Expressions.Num(), 0);

    auto RedirectAdd = BaseParams(MaterialPath);
    RedirectAdd->SetStringField(TEXT("expression_class"), TEXT("Noise"));
    auto RedirectProperties = MakeShared<FJsonObject>();
    RedirectProperties->SetStringField(TEXT("NoiseFunction"), TEXT("NOISEFUNCTION_Perlin"));
    RedirectAdd->SetObjectField(TEXT("properties"), RedirectProperties);
    auto RedirectResult = Commands.HandleCommand(TEXT("add_material_expression"), RedirectAdd);
    TestFalse(TEXT("legacy enum name redirect is accepted"), IsError(RedirectResult));
    for (UMaterialExpression* Candidate : Material->Expressions)
        if (auto Noise = Cast<UMaterialExpressionNoise>(Candidate))
            TestEqual(TEXT("legacy enum name resolves to its current member"), static_cast<int32>(Noise->NoiseFunction), static_cast<int32>(NOISEFUNCTION_GradientTex));
    if (!IsError(RedirectResult))
    {
        auto RemoveNoise = BaseParams(MaterialPath);
        RemoveNoise->SetStringField(TEXT("node_id"), Payload(RedirectResult)->GetStringField(TEXT("node_id")));
        TestFalse(TEXT("redirect test node removed"), IsError(Commands.HandleCommand(TEXT("delete_material_expression"), RemoveNoise)));
    }

    TSharedPtr<FJsonObject> AddParams = BaseParams(MaterialPath);
    AddParams->SetStringField(TEXT("expression_class"), TEXT("ScalarParameter"));
    AddParams->SetStringField(TEXT("parameter_name"), TEXT("RoughnessScale"));
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetNumberField(TEXT("DefaultValue"), 0.35f);
    AddParams->SetObjectField(TEXT("properties"), Properties);
    TSharedPtr<FJsonObject> AddResult = Commands.HandleCommand(TEXT("add_material_expression"), AddParams);
    if (!TestFalse(TEXT("add_material_expression succeeded"), IsError(AddResult)))
    {
        AddError(ErrorText(AddResult));
        return false;
    }
    FString ScalarNodeId;
    Payload(AddResult)->TryGetStringField(TEXT("node_id"), ScalarNodeId);
    if (!TestFalse(TEXT("add returned a node id"), ScalarNodeId.IsEmpty()))
    {
        return false;
    }

    UMaterialExpressionScalarParameter* ScalarNode = nullptr;
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        if (UMaterialExpressionScalarParameter* Candidate = Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            ScalarNode = Candidate;
            break;
        }
    }
    if (!TestNotNull(TEXT("scalar parameter expression exists in the material"), ScalarNode))
    {
        return false;
    }
    TestEqual(TEXT("parameter name written"), ScalarNode->ParameterName.ToString(), FString(TEXT("RoughnessScale")));
    TestEqual(TEXT("default value written"), ScalarNode->DefaultValue, 0.35f);

    TestTrue(TEXT("new material records undo transactions"), Material->HasAnyFlags(RF_Transactional));
    const int32 CountBeforeUndo = Material->Expressions.Num();
    TestTrue(TEXT("editor can undo expression creation"), GEditor->UndoTransaction());
    TestEqual(TEXT("undo removes the created expression from the material"), Material->Expressions.Num(), CountBeforeUndo - 1);
    TestTrue(TEXT("editor can redo expression creation"), GEditor->RedoTransaction());
    TestEqual(TEXT("redo restores the expression"), Material->Expressions.Num(), CountBeforeUndo);

    TSharedPtr<FJsonObject> ConnectPropertyParams = BaseParams(MaterialPath);
    ConnectPropertyParams->SetStringField(TEXT("from_node_id"), ScalarNodeId);
    ConnectPropertyParams->SetStringField(TEXT("property"), TEXT("Roughness"));
    TSharedPtr<FJsonObject> ConnectPropertyResult =
        Commands.HandleCommand(TEXT("connect_material_property"), ConnectPropertyParams);
    if (!TestFalse(TEXT("connect_material_property succeeded"), IsError(ConnectPropertyResult)))
    {
        AddError(ErrorText(ConnectPropertyResult));
        return false;
    }
    TestTrue(TEXT("Roughness input now reads the scalar node"), Material->Roughness.Expression == ScalarNode);

    TSharedPtr<FJsonObject> AddConstantParams = BaseParams(MaterialPath);
    AddConstantParams->SetStringField(TEXT("expression_class"), TEXT("Constant3Vector"));
    TSharedPtr<FJsonObject> ConstantProperties = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Color;
    Color.Add(MakeShared<FJsonValueNumber>(0.1));
    Color.Add(MakeShared<FJsonValueNumber>(0.2));
    Color.Add(MakeShared<FJsonValueNumber>(0.3));
    ConstantProperties->SetArrayField(TEXT("Constant"), Color);
    AddConstantParams->SetObjectField(TEXT("properties"), ConstantProperties);
    TSharedPtr<FJsonObject> AddConstantResult = Commands.HandleCommand(TEXT("add_material_expression"), AddConstantParams);
    if (!TestFalse(TEXT("second expression added"), IsError(AddConstantResult)))
    {
        AddError(ErrorText(AddConstantResult));
        return false;
    }
    FString ConstantNodeId;
    Payload(AddConstantResult)->TryGetStringField(TEXT("node_id"), ConstantNodeId);

    UMaterialExpressionConstant3Vector* ConstantNode = nullptr;
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        if (UMaterialExpressionConstant3Vector* Candidate = Cast<UMaterialExpressionConstant3Vector>(Expression))
        {
            ConstantNode = Candidate;
            break;
        }
    }
    if (!TestNotNull(TEXT("constant expression exists"), ConstantNode))
    {
        return false;
    }
    TestEqual(TEXT("constant colour written"), ConstantNode->Constant.B, 0.3f);

    TSharedPtr<FJsonObject> AddMultiplyParams = BaseParams(MaterialPath);
    AddMultiplyParams->SetStringField(TEXT("expression_class"), TEXT("Multiply"));
    TSharedPtr<FJsonObject> AddMultiplyResult = Commands.HandleCommand(TEXT("add_material_expression"), AddMultiplyParams);
    if (!TestFalse(TEXT("multiply expression added"), IsError(AddMultiplyResult)))
    {
        AddError(ErrorText(AddMultiplyResult));
        return false;
    }
    FString MultiplyNodeId;
    Payload(AddMultiplyResult)->TryGetStringField(TEXT("node_id"), MultiplyNodeId);

    TSharedPtr<FJsonObject> ConnectParams = BaseParams(MaterialPath);
    ConnectParams->SetStringField(TEXT("from_node_id"), ConstantNodeId);
    ConnectParams->SetStringField(TEXT("to_node_id"), MultiplyNodeId);
    ConnectParams->SetStringField(TEXT("to_input"), TEXT("A"));
    TSharedPtr<FJsonObject> ConnectResult = Commands.HandleCommand(TEXT("connect_material_expressions"), ConnectParams);
    if (!TestFalse(TEXT("connect_material_expressions succeeded"), IsError(ConnectResult)))
    {
        AddError(ErrorText(ConnectResult));
        return false;
    }

    UMaterialExpressionMultiply* MultiplyNode = nullptr;
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        if (UMaterialExpressionMultiply* Candidate = Cast<UMaterialExpressionMultiply>(Expression))
        {
            MultiplyNode = Candidate;
            break;
        }
    }
    if (!TestNotNull(TEXT("multiply node exists"), MultiplyNode))
    {
        return false;
    }
    ConnectParams->SetStringField(TEXT("to_input"), FString::ChrN(NAME_SIZE, TEXT('X')));
    TestTrue(TEXT("oversized input name is rejected before FName construction"), IsError(Commands.HandleCommand(TEXT("connect_material_expressions"), ConnectParams)));
    ConnectParams->SetStringField(TEXT("to_input"), TEXT("A"));
    TestEqual(TEXT("multiply node id matches the command reply"), NodeIdString(MultiplyNode), MultiplyNodeId);
    FExpressionInput* MultiplyA = MultiplyNode->GetInputs().Num() > 0 ? MultiplyNode->GetInputs()[0] : nullptr;
    if (!TestNotNull(TEXT("multiply node has an input"), MultiplyA))
    {
        return false;
    }
    TestTrue(TEXT("A input reads the constant node"), MultiplyA->Expression == ConstantNode);

    TSharedPtr<FJsonObject> DisconnectParams = BaseParams(MaterialPath);
    DisconnectParams->SetStringField(TEXT("to_node_id"), MultiplyNodeId);
    DisconnectParams->SetStringField(TEXT("from_node_id"), ConstantNodeId);
    DisconnectParams->SetStringField(TEXT("to_input"), TEXT("A"));
    TSharedPtr<FJsonObject> DisconnectResult =
        Commands.HandleCommand(TEXT("disconnect_material_expressions"), DisconnectParams);
    if (!TestFalse(TEXT("disconnect_material_expressions succeeded"), IsError(DisconnectResult)))
    {
        AddError(ErrorText(DisconnectResult));
        return false;
    }
    TestTrue(TEXT("A input is cleared"), MultiplyA->Expression == nullptr);
    TestTrue(TEXT("disconnect marks the material package dirty"), Material->GetOutermost()->IsDirty());

    ConnectParams->SetNumberField(TEXT("from_output_index"), 999);
    TestTrue(TEXT("out-of-range output is rejected"), IsError(Commands.HandleCommand(TEXT("connect_material_expressions"), ConnectParams)));
    TestTrue(TEXT("rejected output leaves the target disconnected"), MultiplyA->Expression == nullptr);

    TSharedPtr<FJsonObject> InvalidSettings = BaseParams(MaterialPath);
    InvalidSettings->SetStringField(TEXT("blend_mode"), TEXT("Masked"));
    InvalidSettings->SetStringField(TEXT("shading_model"), TEXT("NotARealModel"));
    TestTrue(TEXT("invalid settings are rejected"), IsError(Commands.HandleCommand(TEXT("set_material_settings"), InvalidSettings)));
    TestTrue(TEXT("rejected settings leave the blend mode unchanged"), Material->BlendMode == BLEND_Opaque);

    TSharedPtr<FJsonObject> GraphResult = Commands.HandleCommand(TEXT("get_material_graph"), BaseParams(MaterialPath));
    if (!TestFalse(TEXT("get_material_graph succeeded"), IsError(GraphResult)))
    {
        AddError(ErrorText(GraphResult));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
    TestTrue(TEXT("graph reports expressions"), Payload(GraphResult)->TryGetArrayField(TEXT("expressions"), Expressions));
    if (Expressions)
    {
        TestEqual(TEXT("three expressions reported"), Expressions->Num(), 3);
    }
    const TArray<TSharedPtr<FJsonValue>>* PropertyInputs = nullptr;
    TestTrue(TEXT("graph reports property inputs"),
        Payload(GraphResult)->TryGetArrayField(TEXT("property_inputs"), PropertyInputs));
    if (PropertyInputs)
    {
        bool bRoughnessReported = false;
        for (const TSharedPtr<FJsonValue>& Entry : *PropertyInputs)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (Entry.IsValid() && Entry->TryGetObject(Object) && Object && Object->IsValid())
            {
                FString Name;
                (*Object)->TryGetStringField(TEXT("property"), Name);
                bRoughnessReported = bRoughnessReported || Name == TEXT("Roughness");
            }
        }
        TestTrue(TEXT("Roughness reported as a connected property"), bRoughnessReported);
    }

    TSharedPtr<FJsonObject> ListResult = Commands.HandleCommand(TEXT("list_material_parameters"), BaseParams(MaterialPath));
    if (!TestFalse(TEXT("list_material_parameters succeeded"), IsError(ListResult)))
    {
        AddError(ErrorText(ListResult));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Scalars = nullptr;
    TestTrue(TEXT("scalar parameters reported"),
        Payload(ListResult)->TryGetArrayField(TEXT("scalar_parameters"), Scalars));
    if (Scalars)
    {
        TestEqual(TEXT("one scalar parameter"), Scalars->Num(), 1);
    }

    TSharedPtr<FJsonObject> FormatParams = BaseParams(MaterialPath);
    TSharedPtr<FJsonObject> FormatResult = Commands.HandleCommand(TEXT("format_material_graph"), FormatParams);
    if (!TestFalse(TEXT("format_material_graph succeeded"), IsError(FormatResult)))
    {
        AddError(ErrorText(FormatResult));
        return false;
    }
    TSharedPtr<FJsonObject> Formatted = Payload(FormatResult);
    int32 FormattedCount = 0;
    Formatted->TryGetNumberField(TEXT("formatted_count"), FormattedCount);
    TestEqual(TEXT("layout placed every expression"), FormattedCount, Material->Expressions.Num());
    // Roughness 的属性输入就是标量节点，它必须落在最右列。
    int32 RightmostX = TNumericLimits<int32>::Lowest();
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        RightmostX = FMath::Max(RightmostX, Expression->MaterialExpressionEditorX);
    }
    TestEqual(TEXT("scalar node sits in the rightmost column"), ScalarNode->MaterialExpressionEditorX, RightmostX);

    TSharedPtr<FJsonObject> DeleteParams = BaseParams(MaterialPath);
    DeleteParams->SetStringField(TEXT("node_id"), ConstantNodeId);
    TSharedPtr<FJsonObject> DeleteResult = Commands.HandleCommand(TEXT("delete_material_expression"), DeleteParams);
    if (!TestFalse(TEXT("delete_material_expression succeeded"), IsError(DeleteResult)))
    {
        AddError(ErrorText(DeleteResult));
        return false;
    }
    bool bConstantStillPresent = false;
    for (UMaterialExpression* Expression : Material->Expressions)
    {
        bConstantStillPresent = bConstantStillPresent || Expression == ConstantNode;
    }
    TestFalse(TEXT("deleted node left the material"), bConstantStillPresent);

    auto SaveEdit = BaseParams(MaterialPath);
    SaveEdit->SetStringField(TEXT("node_id"), ScalarNodeId);
    SaveEdit->SetBoolField(TEXT("defer_save"), false);
    auto SavedProperties = MakeShared<FJsonObject>();
    SavedProperties->SetStringField(TEXT("Desc"), TEXT("Saved through the material tool"));
    SaveEdit->SetObjectField(TEXT("properties"), SavedProperties);
    auto SavedEdit = Commands.HandleCommand(TEXT("set_material_expression_property"), SaveEdit);
    TestFalse(TEXT("default material property edit saves successfully"), IsError(SavedEdit));
    const FString Filename = FPackageName::LongPackageNameToFilename(MaterialPath, FPackageName::GetAssetPackageExtension());
    TestTrue(TEXT("material package exists on disk"), FPaths::FileExists(Filename));
    TestFalse(TEXT("saved material package is clean"), Material->GetOutermost()->IsDirty());

    auto BadValueParams = BaseParams(MaterialPath);
    BadValueParams->SetStringField(TEXT("node_id"), ScalarNodeId);
    auto BadValueProperties = MakeShared<FJsonObject>();
    BadValueProperties->SetNumberField(TEXT("DefaultValue"), 1e100);
    BadValueParams->SetObjectField(TEXT("properties"), BadValueProperties);
    const float PreviousValue = ScalarNode->DefaultValue;
    TestTrue(TEXT("overflowing property update is rejected"), IsError(Commands.HandleCommand(TEXT("set_material_expression_property"), BadValueParams)));
    TestEqual(TEXT("rejected overflow preserves the previous float"), ScalarNode->DefaultValue, PreviousValue);
    TestFalse(TEXT("rejected overflow does not dirty the saved package"), Material->GetOutermost()->IsDirty());

    SavedProperties->SetStringField(TEXT("Desc"), TEXT("Must not be applied"));
    SavedProperties->SetStringField(TEXT("NoSuchProperty"), TEXT("invalid"));
    TestTrue(TEXT("invalid property batch is rejected"), IsError(Commands.HandleCommand(TEXT("set_material_expression_property"), SaveEdit)));
    TestEqual(TEXT("rejected property batch preserves the prior value"), ScalarNode->Desc, FString(TEXT("Saved through the material tool")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPMaterialInstanceEditingTest,
    "UnrealMCP.Materials.MaterialInstanceParameters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPMaterialInstanceEditingTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPMaterialTestHelpers;

    FString MaterialName;
    const FString MaterialPath = MakeAssetPath(TEXT("MCP_MI_Parent"), MaterialName);
    FString InstanceName;
    const FString InstancePath = MakeAssetPath(TEXT("MCP_MI_Instance"), InstanceName);
    DeleteTestAsset(InstancePath);
    DeleteTestAsset(MaterialPath);
    ON_SCOPE_EXIT
    {
        DeleteTestAsset(InstancePath);
        DeleteTestAsset(MaterialPath);
    };

    FUnrealMCPMaterialCommands Commands;

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("material_name"), MaterialName);
    CreateParams->SetStringField(TEXT("save_path"), TestPath);
    CreateParams->SetBoolField(TEXT("defer_save"), true);
    if (!TestFalse(TEXT("parent material created"),
        IsError(Commands.HandleCommand(TEXT("create_material"), CreateParams))))
    {
        return false;
    }

    TArray<TPair<FString, FString>> ParameterTypes;
    ParameterTypes.Emplace(TEXT("ScalarParameter"), TEXT("Glow"));
    ParameterTypes.Emplace(TEXT("VectorParameter"), TEXT("Tint"));
    ParameterTypes.Emplace(TEXT("StaticSwitchParameter"), TEXT("Enabled"));
    for (const TPair<FString, FString>& Pair : ParameterTypes)
    {
        TSharedPtr<FJsonObject> AddParams = BaseParams(MaterialPath);
        AddParams->SetStringField(TEXT("expression_class"), Pair.Key);
        AddParams->SetStringField(TEXT("parameter_name"), Pair.Value);
        if (!TestFalse(FString::Printf(TEXT("%s added"), *Pair.Value),
            IsError(Commands.HandleCommand(TEXT("add_material_expression"), AddParams))))
        {
            return false;
        }
    }

    TSharedPtr<FJsonObject> CreateInstanceParams = MakeShared<FJsonObject>();
    CreateInstanceParams->SetStringField(TEXT("instance_name"), InstanceName);
    CreateInstanceParams->SetStringField(TEXT("parent_material_path"), MaterialPath);
    CreateInstanceParams->SetStringField(TEXT("save_path"), TestPath);
    CreateInstanceParams->SetBoolField(TEXT("defer_save"), true);
    TSharedPtr<FJsonObject> CreateInstanceResult =
        Commands.HandleCommand(TEXT("create_material_instance"), CreateInstanceParams);
    if (!TestFalse(TEXT("create_material_instance succeeded"), IsError(CreateInstanceResult)))
    {
        AddError(ErrorText(CreateInstanceResult));
        return false;
    }
    UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(InstancePath));
    if (!TestNotNull(TEXT("instance loads back after creation"), Instance))
    {
        return false;
    }
    TestEqual(TEXT("instance parent is the source material"),
        Instance->Parent ? Instance->Parent->GetPathName() : FString(), FString(MaterialPath + TEXT(".") + MaterialName));

    // 写标量覆盖值，回读必须一致。
    TSharedPtr<FJsonObject> SetScalarParams = MakeShared<FJsonObject>();
    SetScalarParams->SetStringField(TEXT("instance_path"), InstancePath);
    SetScalarParams->SetStringField(TEXT("param_name"), TEXT("Glow"));
    SetScalarParams->SetNumberField(TEXT("scalar"), 2.5f);
    SetScalarParams->SetBoolField(TEXT("defer_save"), true);
    TSharedPtr<FJsonObject> SetScalarResult =
        Commands.HandleCommand(TEXT("set_material_instance_parameter"), SetScalarParams);
    if (!TestFalse(TEXT("scalar override accepted"), IsError(SetScalarResult)))
    {
        AddError(ErrorText(SetScalarResult));
        return false;
    }
    SetScalarParams->SetStringField(TEXT("param_name"), FString::ChrN(NAME_SIZE, TEXT('X')));
    TestTrue(TEXT("oversized instance parameter name is rejected"), IsError(Commands.HandleCommand(TEXT("set_material_instance_parameter"), SetScalarParams)));
    SetScalarParams->SetStringField(TEXT("param_name"), TEXT("Glow"));
    // K2_GetScalarParameterValue 没有 ENGINE_API，跨模块调用会链接失败；用导出过的编辑库接口回读。
    const float ReadBackGlow = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, TEXT("Glow"));
    TestEqual(TEXT("scalar override reads back"), ReadBackGlow, 2.5f);

    TSharedPtr<FJsonObject> SwitchParams = MakeShared<FJsonObject>();
    SwitchParams->SetStringField(TEXT("instance_path"), InstancePath);
    SwitchParams->SetStringField(TEXT("param_name"), TEXT("Enabled"));
    SwitchParams->SetBoolField(TEXT("static_switch"), true);
    SwitchParams->SetBoolField(TEXT("defer_save"), true);
    auto SwitchResult = Commands.HandleCommand(TEXT("set_material_instance_parameter"), SwitchParams);
    TestFalse(TEXT("inherited static switch override accepted"), IsError(SwitchResult));
    bool bSwitchOverride = false;
    FStaticParameterSet StaticParameters;
    Instance->GetStaticParameterValues(StaticParameters);
    for (const FStaticSwitchParameter& Entry : StaticParameters.StaticSwitchParameters)
        bSwitchOverride |= Entry.ParameterInfo.Name == TEXT("Enabled") && Entry.bOverride && Entry.Value;
    TestTrue(TEXT("static switch value and override read back"), bSwitchOverride);

    // 未知参数名必须被拒绝，而不是静默丢弃。
    TSharedPtr<FJsonObject> UnknownParams = MakeShared<FJsonObject>();
    UnknownParams->SetStringField(TEXT("instance_path"), InstancePath);
    UnknownParams->SetStringField(TEXT("param_name"), TEXT("NoSuchParameter"));
    UnknownParams->SetNumberField(TEXT("scalar"), 1.0f);
    UnknownParams->SetBoolField(TEXT("defer_save"), true);
    TestTrue(TEXT("unknown parameter name is rejected"),
        IsError(Commands.HandleCommand(TEXT("set_material_instance_parameter"), UnknownParams)));

    // 回读接口要报出父材质和已写入的覆盖值。
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("instance_path"), InstancePath);
    TSharedPtr<FJsonObject> GetResult = Commands.HandleCommand(TEXT("get_material_instance_parameters"), GetParams);
    if (!TestFalse(TEXT("get_material_instance_parameters succeeded"), IsError(GetResult)))
    {
        AddError(ErrorText(GetResult));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
    TestTrue(TEXT("scalar overrides reported"),
        Payload(GetResult)->TryGetArrayField(TEXT("scalar_parameters"), Overrides));
    if (Overrides)
    {
        bool bGlowReported = false;
        for (const TSharedPtr<FJsonValue>& Entry : *Overrides)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (Entry.IsValid() && Entry->TryGetObject(Object) && Object && Object->IsValid())
            {
                FString Name;
                (*Object)->TryGetStringField(TEXT("name"), Name);
                bGlowReported = bGlowReported || Name == TEXT("Glow");
            }
        }
        TestTrue(TEXT("Glow override listed"), bGlowReported);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
