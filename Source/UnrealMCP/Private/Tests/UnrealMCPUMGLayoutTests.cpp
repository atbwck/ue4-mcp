#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "EditorAssetLibrary.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUMGLayoutTest, "UnrealMCP.Layout.WidgetComposition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPUMGLayoutTest::RunTest(const FString& Parameters)
{
    const FString Folder = TEXT("/Game/UnrealMCPLayoutTests");
    const FString Host = Folder + TEXT("/WBP_LayoutHost");
    const FString Child = Folder + TEXT("/WBP_LayoutChild");
    if (UEditorAssetLibrary::DoesAssetExist(Host) || UEditorAssetLibrary::DoesAssetExist(Child))
    {
        AddError(TEXT("Layout test assets already exist; refusing to replace them"));
        return false;
    }
    ON_SCOPE_EXIT { UEditorAssetLibrary::DeleteAsset(Child); UEditorAssetLibrary::DeleteAsset(Host); };
    FUnrealMCPUMGCommands Commands;
    for (const FString& Name : {FString(TEXT("WBP_LayoutHost")), FString(TEXT("WBP_LayoutChild"))})
    {
        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Name);
        P->SetStringField(TEXT("path"), Folder);
        if (Commands.HandleCommand(TEXT("create_umg_widget_blueprint"), P)->HasField(TEXT("error")))
        {
            AddError(TEXT("Could not create test Widget Blueprint"));
            return false;
        }
    }
    auto Add = [&](const FString& Blueprint, const FString& Name, const FString& Class, const FString& Parent,
                   const TSharedPtr<FJsonObject>& Layout, const TSharedPtr<FJsonObject>& Properties)
    {
        auto P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("blueprint_name"), Blueprint);
        P->SetStringField(TEXT("widget_name"), Name);
        P->SetStringField(TEXT("widget_class"), Class);
        if (!Parent.IsEmpty()) { P->SetStringField(TEXT("parent_name"), Parent); }
        P->SetObjectField(TEXT("layout"), Layout);
        P->SetObjectField(TEXT("properties"), Properties);
        return Commands.HandleCommand(TEXT("add_widget_to_widget"), P);
    };
    auto Empty = MakeShared<FJsonObject>();
    auto Canvas = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Position = {MakeShared<FJsonValueNumber>(64), MakeShared<FJsonValueNumber>(128)};
    TArray<TSharedPtr<FJsonValue>> Size = {MakeShared<FJsonValueNumber>(640), MakeShared<FJsonValueNumber>(480)};
    Canvas->SetArrayField(TEXT("position"), Position);
    Canvas->SetArrayField(TEXT("size"), Size);
    TestFalse(TEXT("Native panel accepted"), Add(Host, TEXT("Column"), TEXT("VerticalBox"), TEXT(""), Canvas, Empty)->HasField(TEXT("error")));
    auto Box = MakeShared<FJsonObject>();
    Box->SetNumberField(TEXT("fill"), 1);
    Box->SetStringField(TEXT("h_align"), TEXT("center"));
    auto Text = MakeShared<FJsonObject>();
    Text->SetStringField(TEXT("Text"), TEXT("Layout title"));
    TestFalse(TEXT("Text in box accepted"), Add(Host, TEXT("Title"), TEXT("TextBlock"), TEXT("Column"), Box, Text)->HasField(TEXT("error")));
    UWidgetBlueprint* BP = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(Host));
    if (!BP) { return false; }
    UTextBlock* Title = Cast<UTextBlock>(BP->WidgetTree->FindWidget(TEXT("Title")));
    if (!Title) { AddError(TEXT("Title missing after compile")); return false; }
    TestEqual(TEXT("Text property retained"), Title->GetText().ToString(), FString(TEXT("Layout title")));
    for (const FString& Class : {FString(TEXT("HorizontalBox")), FString(TEXT("Overlay")), FString(TEXT("Border")), FString(TEXT("SizeBox"))})
    {
        const FString Name = TEXT("Panel_") + Class;
        TestFalse(TEXT("Panel created"), Add(Host, Name, Class, TEXT("Column"), Empty, Empty)->HasField(TEXT("error")));
        TestFalse(TEXT("Panel child created"), Add(Host, Name + TEXT("_Child"), TEXT("TextBlock"), Name, Empty, Empty)->HasField(TEXT("error")));
    }
    TestTrue(TEXT("Single-child container rejects second child"), Add(Host, TEXT("Excess"), TEXT("TextBlock"), TEXT("Panel_Border"), Empty, Empty)->HasField(TEXT("error")));
    TestTrue(TEXT("Duplicate name rejected"), Add(Host, TEXT("Title"), TEXT("TextBlock"), TEXT("Column"), Empty, Empty)->HasField(TEXT("error")));
    auto Invalid = MakeShared<FJsonObject>();
    Invalid->SetNumberField(TEXT("unknown_layout_field"), 1);
    TestTrue(TEXT("Invalid layout rejected"), Add(Host, TEXT("BadLayout"), TEXT("TextBlock"), TEXT("Column"), Invalid, Empty)->HasField(TEXT("error")));
    TestNull(TEXT("Invalid layout leaves no widget"), BP->WidgetTree->FindWidget(TEXT("BadLayout")));
    TestFalse(TEXT("Custom widget child supported"), Add(Host, TEXT("Reusable"), Child, TEXT("Column"), Empty, Empty)->HasField(TEXT("error")));
    TestTrue(TEXT("Direct recursion rejected"), Add(Host, TEXT("SelfCycle"), Host, TEXT("Column"), Empty, Empty)->HasField(TEXT("error")));
    TestTrue(TEXT("Indirect recursion rejected"), Add(Child, TEXT("IndirectCycle"), Host, TEXT(""), Empty, Empty)->HasField(TEXT("error")));
    auto Query = MakeShared<FJsonObject>();
    Query->SetStringField(TEXT("blueprint_name"), Host);
    auto Tree = Commands.HandleCommand(TEXT("get_widget_tree"), Query);
    TestFalse(TEXT("Host hierarchy readable"), Tree->HasField(TEXT("error")));
    Query->SetStringField(TEXT("path"), Folder);
    auto List = Commands.HandleCommand(TEXT("list_widget_blueprints"), Query);
    TestFalse(TEXT("Widget candidates discoverable"), List->HasField(TEXT("error")));
    TestEqual(TEXT("Two discovered widgets"), List->GetObjectField(TEXT("data"))->GetArrayField(TEXT("blueprints")).Num(), 2);
    return !HasAnyErrors();
}
#endif
