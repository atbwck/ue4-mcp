#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/NamedSlotInterface.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "Fonts/SlateFontInfo.h"

namespace
{
    enum class ELayoutKind { Unsupported, Canvas, Box, Overlay, Content };

    struct FLayout
    {
        FVector2D Position = FVector2D::ZeroVector;
        FVector2D Size = FVector2D(100, 30);
        FVector2D Alignment = FVector2D::ZeroVector;
        FAnchors Anchors;
        FMargin Padding = FMargin(0);
        FSlateChildSize ChildSize;
        EHorizontalAlignment HAlign = HAlign_Fill;
        EVerticalAlignment VAlign = VAlign_Fill;
        int32 ZOrder = 0;
        bool bAutoSize = false;
    };

    TSharedPtr<FJsonObject> LayoutError(const FString& Message)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Message);
    }

    ELayoutKind LayoutKind(const UPanelWidget* Panel)
    {
        if (!Panel) { return ELayoutKind::Unsupported; }
        if (Panel->IsA<UCanvasPanel>()) { return ELayoutKind::Canvas; }
        if (Panel->IsA<UHorizontalBox>() || Panel->IsA<UVerticalBox>()) { return ELayoutKind::Box; }
        if (Panel->IsA<UOverlay>()) { return ELayoutKind::Overlay; }
        const bool bBorderOrSize = Panel->IsA<UBorder>() || Panel->IsA<USizeBox>();
        if (bBorderOrSize || Panel->IsA<UButton>()) { return ELayoutKind::Content; }
        return ELayoutKind::Unsupported;
    }

    bool Number(const TSharedPtr<FJsonValue>& Value, double& Out)
    {
        if (!Value.IsValid()) { return false; }
        if (Value->Type != EJson::Number) { return false; }
        Out = Value->AsNumber();
        return FMath::IsFinite(Out) && FMath::Abs(Out) <= MAX_flt;
    }

    bool Numbers(const TSharedPtr<FJsonValue>& Value, int32 Count, TArray<double>& Out)
    {
        if (!Value.IsValid()) { return false; }
        if (Value->Type != EJson::Array) { return false; }
        const auto& Values = Value->AsArray();
        if (Values.Num() != Count) { return false; }
        Out.Reset();
        for (const auto& Element : Values)
        {
            double N = 0;
            if (!Number(Element, N)) { return false; }
            Out.Add(N);
        }
        return true;
    }

    bool ParseAlignment(const TSharedPtr<FJsonValue>& Value, bool bHorizontal, int32& Out)
    {
        if (!Value.IsValid()) { return false; }
        if (Value->Type != EJson::String) { return false; }
        const FString Name = Value->AsString().ToLower();
        if (Name == TEXT("fill")) { Out = 0; return true; }
        if (Name == TEXT("center")) { Out = 2; return true; }
        if (Name == (bHorizontal ? TEXT("left") : TEXT("top"))) { Out = 1; return true; }
        if (Name == (bHorizontal ? TEXT("right") : TEXT("bottom"))) { Out = 3; return true; }
        return false;
    }

    bool ParseLayout(const TSharedPtr<FJsonObject>& Json, ELayoutKind Kind, FLayout& Out, FString& Why)
    {
        if (Kind == ELayoutKind::Unsupported)
        {
            Why = TEXT("Unsupported parent panel type");
            return false;
        }
        for (const auto& Field : Json->Values)
        {
            const FString& Key = Field.Key;
            const auto& Value = Field.Value;
            TArray<double> V;
            double N = 0;
            bool bValid = false;
            if (Kind == ELayoutKind::Canvas)
            {
                if (Key == TEXT("position") || Key == TEXT("size") || Key == TEXT("alignment"))
                {
                    bValid = Numbers(Value, 2, V);
                    if (bValid)
                    {
                        const FVector2D Pair(V[0], V[1]);
                        if (Key == TEXT("position")) { Out.Position = Pair; }
                        if (Key == TEXT("size"))
                        {
                            bValid = V[0] > 0 && V[1] > 0;
                            Out.Size = Pair;
                        }
                        if (Key == TEXT("alignment"))
                        {
                            bValid = V[0] >= 0 && V[0] <= 1;
                            bValid = bValid && V[1] >= 0 && V[1] <= 1;
                            Out.Alignment = Pair;
                        }
                    }
                }
                else if (Key == TEXT("anchors"))
                {
                    bValid = Numbers(Value, 4, V);
                    if (bValid)
                    {
                        for (double Anchor : V) { bValid = bValid && Anchor >= 0 && Anchor <= 1; }
                        bValid = bValid && V[0] <= V[2] && V[1] <= V[3];
                        Out.Anchors = FAnchors(V[0], V[1], V[2], V[3]);
                    }
                }
                else if (Key == TEXT("z_order"))
                {
                    bValid = Number(Value, N);
                    if (bValid)
                    {
                        bValid = N >= MIN_int32 && N <= MAX_int32;
                        bValid = bValid && N == FMath::FloorToDouble(N);
                        if (bValid) { Out.ZOrder = static_cast<int32>(N); }
                    }
                }
                else if (Key == TEXT("autosize"))
                {
                    bValid = Value.IsValid();
                    if (bValid)
                    {
                        bValid = Value->Type == EJson::Boolean;
                        if (bValid) { Out.bAutoSize = Value->AsBool(); }
                    }
                }
            }
            else
            {
                if (Key == TEXT("padding"))
                {
                    bValid = Numbers(Value, 4, V);
                    if (bValid)
                    {
                        for (double Pad : V) { bValid = bValid && Pad >= 0; }
                        Out.Padding = FMargin(V[0], V[1], V[2], V[3]);
                    }
                }
                else if (Key == TEXT("h_align") || Key == TEXT("v_align"))
                {
                    int32 Align = 0;
                    const bool bHorizontal = Key == TEXT("h_align");
                    bValid = ParseAlignment(Value, bHorizontal, Align);
                    if (bValid)
                    {
                        if (bHorizontal) { Out.HAlign = static_cast<EHorizontalAlignment>(Align); }
                        else { Out.VAlign = static_cast<EVerticalAlignment>(Align); }
                    }
                }
                else if (Key == TEXT("fill") && Kind == ELayoutKind::Box)
                {
                    if (Value.IsValid())
                    {
                        if (Value->Type == EJson::Null)
                        {
                            Out.ChildSize.SizeRule = ESlateSizeRule::Automatic;
                            bValid = true;
                        }
                        else
                        {
                            bValid = Number(Value, N);
                            bValid = bValid && N > 0;
                            if (bValid)
                            {
                                Out.ChildSize.SizeRule = ESlateSizeRule::Fill;
                                Out.ChildSize.Value = N;
                            }
                        }
                    }
                }
            }
            if (!bValid)
            {
                Why = FString::Printf(TEXT("Invalid or unsupported layout field '%s' for this parent"), *Key);
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonValue> Array(std::initializer_list<double> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (double Value : Values) { Items.Add(MakeShared<FJsonValueNumber>(Value)); }
        return MakeShared<FJsonValueArray>(Items);
    }

    template<typename T>
    T ReadStruct(UObject* Object, const TCHAR* Name, const T& Default)
    {
        if (!Object) { return Default; }
        FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), Name);
        if (!Property) { return Default; }
        return *Property->ContainerPtrToValuePtr<T>(Object);
    }

    uint8 ReadByte(UObject* Object, const TCHAR* Name)
    {
        if (!Object) { return 0; }
        FByteProperty* Property = FindFProperty<FByteProperty>(Object->GetClass(), Name);
        if (!Property) { return 0; }
        return Property->GetPropertyValue_InContainer(Object);
    }

    TSharedPtr<FJsonObject> SlotJson(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        UPanelSlot* Slot = Widget->Slot;
        if (!Slot) { return Json; }
        if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
        {
            const FVector2D P = Canvas->GetPosition(), S = Canvas->GetSize(), A = Canvas->GetAlignment();
            const FAnchors Anchors = Canvas->GetAnchors();
            Json->SetField(TEXT("position"), Array({P.X, P.Y}));
            Json->SetField(TEXT("size"), Array({S.X, S.Y}));
            Json->SetField(TEXT("alignment"), Array({A.X, A.Y}));
            Json->SetField(TEXT("anchors"), Array({Anchors.Minimum.X, Anchors.Minimum.Y, Anchors.Maximum.X, Anchors.Maximum.Y}));
            Json->SetNumberField(TEXT("z_order"), Canvas->GetZOrder());
            Json->SetBoolField(TEXT("autosize"), Canvas->GetAutoSize());
            return Json;
        }
        const ELayoutKind Kind = LayoutKind(Slot->Parent);
        if (Kind == ELayoutKind::Unsupported)
        {
            Json->SetBoolField(TEXT("supported"), false);
            return Json;
        }
        // Border 的槽设置实际保存在父控件上，读取有效值而非过期槽副本。
        UObject* LayoutOwner = Slot;
        if (Cast<UBorderSlot>(Slot)) { LayoutOwner = Slot->Parent; }
        const FMargin Padding = ReadStruct<FMargin>(LayoutOwner, TEXT("Padding"), FMargin(0));
        Json->SetField(TEXT("padding"), Array({Padding.Left, Padding.Top, Padding.Right, Padding.Bottom}));
        static const TCHAR* HNames[] = {TEXT("fill"), TEXT("left"), TEXT("center"), TEXT("right")};
        static const TCHAR* VNames[] = {TEXT("fill"), TEXT("top"), TEXT("center"), TEXT("bottom")};
        const uint8 H = ReadByte(LayoutOwner, TEXT("HorizontalAlignment"));
        const uint8 V = ReadByte(LayoutOwner, TEXT("VerticalAlignment"));
        Json->SetStringField(TEXT("h_align"), H < 4 ? HNames[H] : TEXT("unknown"));
        Json->SetStringField(TEXT("v_align"), V < 4 ? VNames[V] : TEXT("unknown"));
        if (Kind == ELayoutKind::Box)
        {
            const FSlateChildSize Size = ReadStruct<FSlateChildSize>(Slot, TEXT("Size"), FSlateChildSize());
            if (Size.SizeRule == ESlateSizeRule::Fill) { Json->SetNumberField(TEXT("fill"), Size.Value); }
            else { Json->SetField(TEXT("fill"), MakeShared<FJsonValueNull>()); }
        }
        return Json;
    }

    struct FHostWidget { UWidget* Widget; FString Parent; };

    void CollectHost(UWidgetTree* Tree, UWidget* Widget, const FString& Parent, TArray<FHostWidget>& Out, TSet<UWidget*>& Seen)
    {
        if (!Widget) { return; }
        // 必须在访问孩子之前检查归属，禁止进入自定义 UserWidget 的外部树。
        if (Widget->GetOuter() != Tree || Seen.Contains(Widget)) { return; }
        Seen.Add(Widget);
        Out.Add({Widget, Parent});
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            for (UWidget* Child : Panel->GetAllChildren()) { CollectHost(Tree, Child, Widget->GetName(), Out, Seen); }
        }
        if (INamedSlotInterface* Named = Cast<INamedSlotInterface>(Widget))
        {
            TArray<FName> Names;
            Named->GetSlotNames(Names);
            for (FName Name : Names) { CollectHost(Tree, Named->GetContentForSlot(Name), Widget->GetName(), Out, Seen); }
        }
    }

    TArray<FHostWidget> HostWidgets(UWidgetTree* Tree)
    {
        TArray<FHostWidget> Result;
        TSet<UWidget*> Seen;
        CollectHost(Tree, Tree->RootWidget, FString(), Result, Seen);
        return Result;
    }

    TSharedPtr<FJsonObject> WidgetJson(const FHostWidget& Entry)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Entry.Widget->GetName());
        Json->SetStringField(TEXT("class"), Entry.Widget->GetClass()->GetPathName());
        if (Entry.Parent.IsEmpty()) { Json->SetField(TEXT("parent"), MakeShared<FJsonValueNull>()); }
        else { Json->SetStringField(TEXT("parent"), Entry.Parent); }
        UPanelSlot* Slot = Entry.Widget->Slot;
        if (Slot) { Json->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName()); }
        else { Json->SetField(TEXT("slot_class"), MakeShared<FJsonValueNull>()); }
        Json->SetObjectField(TEXT("layout"), SlotJson(Entry.Widget));
        return Json;
    }

    bool CheckDependencies(UClass* Class, UWidgetBlueprint* Host, TSet<UClass*>& Active, TSet<UClass*>& Done, FString& Why)
    {
        if (!Class) { return true; }
        if (Done.Contains(Class)) { return true; }
        if (Class->ClassGeneratedBy == Host || Active.Contains(Class))
        {
            Why = TEXT("Recursive widget dependency through a widget tree or parent class");
            return false;
        }
        Active.Add(Class);
        if (!CheckDependencies(Class->GetSuperClass(), Host, Active, Done, Why)) { return false; }
        TArray<UWidgetTree*> Trees;
        if (UWidgetBlueprint* Source = Cast<UWidgetBlueprint>(Class->ClassGeneratedBy))
        {
            if (Source->WidgetTree) { Trees.AddUnique(Source->WidgetTree); }
        }
        if (UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Class))
        {
            if (Generated->GetWidgetTreeArchetype()) { Trees.AddUnique(Generated->GetWidgetTreeArchetype()); }
        }
        for (UWidgetTree* Tree : Trees)
        {
            // 源树与生成类模板都检查；共享依赖通过 Done 去重，只有活动路径上的重复才是环。
            TArray<UObject*> Objects;
            GetObjectsWithOuter(Tree, Objects, false);
            for (UObject* Object : Objects)
            {
                if (UWidget* Widget = Cast<UWidget>(Object))
                {
                    if (!CheckDependencies(Widget->GetClass(), Host, Active, Done, Why)) { return false; }
                }
            }
        }
        Active.Remove(Class);
        Done.Add(Class);
        return true;
    }

    UClass* ResolveClass(const FString& Path)
    {
        if (Path.StartsWith(TEXT("/Script/"))) { return LoadClass<UWidget>(nullptr, *Path); }
        if (!Path.StartsWith(TEXT("/Game/")))
        {
            if (Path.Contains(TEXT("/")) || Path.Contains(TEXT("."))) { return nullptr; }
            return LoadClass<UWidget>(nullptr, *(TEXT("/Script/UMG.") + Path));
        }
        if (UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(Path)))
        {
            if (Blueprint->Status == BS_Error) { return nullptr; }
            return Blueprint->GeneratedClass;
        }
        return LoadClass<UWidget>(nullptr, *Path);
    }

    bool ValidProperty(UClass* Class, const FString& Name, const TSharedPtr<FJsonValue>& Value, FString& Why)
    {
        FProperty* Property = Class->FindPropertyByName(*Name);
        if (!Property)
        {
            Why = FString::Printf(TEXT("Property not found: %s"), *Name);
            return false;
        }
        const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
        const bool bStructural = Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_EditConst);
        if (!bEditable || bStructural || Property->ArrayDim != 1)
        {
            Why = FString::Printf(TEXT("Property is not a supported editable value: %s"), *Name);
            return false;
        }
        if (Property->IsA<FTextProperty>())
        {
            if (!Value.IsValid()) { Why = TEXT("Text value is required"); return false; }
            if (Value->Type != EJson::String) { Why = TEXT("Text property requires a string"); return false; }
            return true;
        }
        // FSlateFontInfo 含字体对象引用，整体写入被拒绝；这里只放行
        // Font.Size 子字段，通过临时对象按结构体成员写入，避免引用属性。
        if (FStructProperty* FontStruct = CastField<FStructProperty>(Property))
        {
            if (FontStruct->Struct->GetFName() == TEXT("SlateFontInfo")
                && Name == TEXT("Font") && Value.IsValid() && Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject>* FontObject = nullptr;
                double FontSize = 0;
                const TSharedPtr<FJsonObject> FontJson = Value->AsObject();
                if (FontJson.IsValid()
                    && FontJson->TryGetObjectField(TEXT("Size"), FontObject)
                    && (*FontObject)->TryGetNumberField(TEXT("value"), FontSize)
                    && FontSize >= 1 && FontSize <= 1000 && FontSize == FMath::FloorToDouble(FontSize)
                    && (*FontObject)->Values.Num() == 1)
                {
                    return true;
                }
                Why = TEXT("Font object supports only Size with a single numeric value");
                return false;
            }
        }
        bool bSupported = Property->IsA<FBoolProperty>() || Property->IsA<FStrProperty>();
        bSupported = bSupported || Property->IsA<FIntProperty>() || Property->IsA<FFloatProperty>();
        bSupported = bSupported || Property->IsA<FByteProperty>() || Property->IsA<FEnumProperty>();
        if (FStructProperty* Struct = CastField<FStructProperty>(Property))
        {
            const UScriptStruct* Type = Struct->Struct;
            int32 Count = 0;
            if (Type == TBaseStructure<FVector2D>::Get()) { Count = 2; }
            if (Type == TBaseStructure<FVector>::Get() || Type == TBaseStructure<FRotator>::Get()) { Count = 3; }
            TArray<double> NumbersValue;
            bSupported = Count > 0 && Numbers(Value, Count, NumbersValue);
        }
        if (!bSupported || !Value.IsValid())
        {
            Why = FString::Printf(TEXT("Unsupported property or value: %s (references and structural properties are not accepted)"), *Name);
            return false;
        }
        if (Property->IsA<FIntProperty>() || Property->IsA<FFloatProperty>())
        {
            double N = 0;
            if (!Number(Value, N)) { Why = TEXT("Numeric properties require finite numbers"); return false; }
            if (Property->IsA<FIntProperty>())
            {
                if (N < MIN_int32 || N > MAX_int32 || N != FMath::FloorToDouble(N))
                {
                    Why = TEXT("Integer property requires an int32 value");
                    return false;
                }
            }
        }
        UEnum* Enum = nullptr;
        if (FByteProperty* Byte = CastField<FByteProperty>(Property)) { Enum = Byte->GetIntPropertyEnum(); }
        if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property)) { Enum = EnumProperty->GetEnum(); }
        if (Enum)
        {
            int64 EnumValue = INDEX_NONE;
            if (Value->Type == EJson::String) { EnumValue = Enum->GetValueByNameString(Value->AsString()); }
            else
            {
                double N = 0;
                if (Number(Value, N))
                {
                    const bool bByteRange = N >= 0 && N <= 255;
                    if (bByteRange && N == FMath::FloorToDouble(N)) { EnumValue = static_cast<int64>(N); }
                }
            }
            if (EnumValue == INDEX_NONE || !Enum->IsValidEnumValue(EnumValue))
            {
                Why = TEXT("Enum property requires a declared enum name or value");
                return false;
            }
        }
        else if (Property->IsA<FByteProperty>())
        {
            double N = 0;
            if (!Number(Value, N)) { Why = TEXT("Byte property requires a number"); return false; }
            if (N < 0 || N > 255 || N != FMath::FloorToDouble(N)) { Why = TEXT("Byte property requires an integer in [0,255]"); return false; }
        }
        return true;
    }

    bool SetWidgetValue(UWidget* Widget, const FString& Name, const TSharedPtr<FJsonValue>& Value, FString& Why)
    {
        if (FTextProperty* Text = FindFProperty<FTextProperty>(Widget->GetClass(), *Name))
        {
            Text->SetPropertyValue_InContainer(Widget, FText::FromString(Value->AsString()));
            return true;
        }
        // Font 只放行 Size 子字段：结构体成员直写，不触碰字体对象引用。
        if (Name == TEXT("Font") && Value.IsValid() && Value->Type == EJson::Object)
        {
            if (FStructProperty* FontStruct = FindFProperty<FStructProperty>(Widget->GetClass(), TEXT("Font")))
            {
                if (FontStruct->Struct->GetFName() == TEXT("SlateFontInfo"))
                {
                    FSlateFontInfo* Font = FontStruct->ContainerPtrToValuePtr<FSlateFontInfo>(Widget);
                    const TSharedPtr<FJsonObject>* FontObject = nullptr;
                    double FontSize = 0;
                    const TSharedPtr<FJsonObject> FontJson = Value->AsObject();
                    if (FontJson.IsValid()
                        && FontJson->TryGetObjectField(TEXT("Size"), FontObject)
                        && (*FontObject)->TryGetNumberField(TEXT("value"), FontSize))
                    {
                        Font->Size = static_cast<int32>(FontSize);
                        return true;
                    }
                }
            }
            Why = TEXT("Font object supports only Size with a single numeric value");
            return false;
        }
        return FUnrealMCPCommonUtils::SetObjectProperty(Widget, Name, Value, Why);
    }

    template<typename T>
    void ApplyCommon(T* Slot, const FLayout& Layout, const TSharedPtr<FJsonObject>& Json)
    {
        if (Json->HasField(TEXT("padding"))) { Slot->SetPadding(Layout.Padding); }
        if (Json->HasField(TEXT("h_align"))) { Slot->SetHorizontalAlignment(Layout.HAlign); }
        if (Json->HasField(TEXT("v_align"))) { Slot->SetVerticalAlignment(Layout.VAlign); }
    }

    bool ApplyLayout(UPanelSlot* Slot, const FLayout& Layout, const TSharedPtr<FJsonObject>& Json)
    {
        if (!Slot) { return false; }
        if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
        {
            if (Json->HasField(TEXT("anchors"))) { Canvas->SetAnchors(Layout.Anchors); }
            if (Json->HasField(TEXT("alignment"))) { Canvas->SetAlignment(Layout.Alignment); }
            if (Json->HasField(TEXT("position"))) { Canvas->SetPosition(Layout.Position); }
            if (Json->HasField(TEXT("size"))) { Canvas->SetSize(Layout.Size); }
            if (Json->HasField(TEXT("z_order"))) { Canvas->SetZOrder(Layout.ZOrder); }
            if (Json->HasField(TEXT("autosize"))) { Canvas->SetAutoSize(Layout.bAutoSize); }
            return true;
        }
        if (UHorizontalBoxSlot* Box = Cast<UHorizontalBoxSlot>(Slot))
        {
            ApplyCommon(Box, Layout, Json);
            if (Json->HasField(TEXT("fill"))) { Box->SetSize(Layout.ChildSize); }
            return true;
        }
        if (UVerticalBoxSlot* Box = Cast<UVerticalBoxSlot>(Slot))
        {
            ApplyCommon(Box, Layout, Json);
            if (Json->HasField(TEXT("fill"))) { Box->SetSize(Layout.ChildSize); }
            return true;
        }
        if (UOverlaySlot* Overlay = Cast<UOverlaySlot>(Slot)) { ApplyCommon(Overlay, Layout, Json); return true; }
        if (UBorderSlot* Border = Cast<UBorderSlot>(Slot)) { ApplyCommon(Border, Layout, Json); return true; }
        if (USizeBoxSlot* Size = Cast<USizeBoxSlot>(Slot)) { ApplyCommon(Size, Layout, Json); return true; }
        if (UButtonSlot* Button = Cast<UButtonSlot>(Slot)) { ApplyCommon(Button, Layout, Json); return true; }
        return false;
    }

    bool Abandon(UObject* Object)
    {
        if (!Object) { return true; }
        // 脱离宿主 Outer，释放名称，保留由 GC 管理的生命周期。
        const FName Name = MakeUniqueObjectName(GetTransientPackage(), Object->GetClass());
        if (!Object->Rename(*Name.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty)) { return false; }
        Object->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
        Object->SetFlags(RF_Transient);
        return true;
    }

    bool RemoveNewWidget(UWidgetTree* Tree, UWidget* Widget)
    {
        if (!Widget) { return true; }
        UPanelSlot* Slot = Widget->Slot;
        Tree->RemoveWidget(Widget);
        Widget->Slot = nullptr;
        bool bClean = true;
        if (Slot)
        {
            Slot->Content = nullptr;
            Slot->Parent = nullptr;
            bClean = Abandon(Slot);
        }
        return Abandon(Widget) && bClean;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleListWidgetBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid()) { return LayoutError(TEXT("Parameters must be an object")); }
    FString Path = TEXT("/Game"), Filter;
    bool bRecursive = true;
    if (Params->HasField(TEXT("path")))
    {
        if (!Params->TryGetStringField(TEXT("path"), Path)) { return LayoutError(TEXT("path must be a string")); }
    }
    Path.RemoveFromEnd(TEXT("/"));
    const bool bGamePath = Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"));
    if (!bGamePath || !FPackageName::IsValidLongPackageName(Path + TEXT("/PathProbe"))) { return LayoutError(TEXT("path must be a valid /Game folder")); }
    if (Params->HasField(TEXT("recursive")))
    {
        if (!Params->TryGetBoolField(TEXT("recursive"), bRecursive)) { return LayoutError(TEXT("recursive must be a boolean")); }
    }
    int32 FilterCount = 0;
    for (const TCHAR* Key : {TEXT("name_filter"), TEXT("name"), TEXT("filter")})
    {
        if (Params->HasField(Key))
        {
            ++FilterCount;
            if (!Params->TryGetStringField(Key, Filter)) { return LayoutError(TEXT("Name filter must be a string")); }
        }
    }
    if (FilterCount > 1) { return LayoutError(TEXT("Use only one of name_filter, name or filter")); }
    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    Module.Get().GetAssetsByPath(FName(*Path), Assets, bRecursive);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.ObjectPath.ToString() < B.ObjectPath.ToString(); });
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& Asset : Assets)
    {
        if (!Filter.IsEmpty() && !Asset.AssetName.ToString().Contains(Filter, ESearchCase::IgnoreCase)) { continue; }
        // 只加载 WidgetBlueprint 资产；父类信息来自蓝图本身，不推断使用频率。
        if (Asset.AssetClass != UWidgetBlueprint::StaticClass()->GetFName()) { continue; }
        UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Asset.GetAsset());
        if (!Blueprint) { return LayoutError(FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *Asset.ObjectPath.ToString())); }
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Item->SetStringField(TEXT("path"), Asset.ObjectPath.ToString());
        if (Blueprint->ParentClass) { Item->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetPathName()); }
        else { Item->SetField(TEXT("parent_class"), MakeShared<FJsonValueNull>()); }
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("path"), Path);
    Data->SetBoolField(TEXT("recursive"), bRecursive);
    Data->SetArrayField(TEXT("blueprints"), Items);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid()) { return LayoutError(TEXT("Parameters must be an object")); }
    FString Name;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), Name) || Name.IsEmpty()) { return LayoutError(TEXT("blueprint_name must be a nonempty name or /Game path")); }
    UWidgetBlueprint* Blueprint = FindLayoutWidgetBlueprint(Name);
    if (!Blueprint) { return LayoutError(TEXT("Widget Blueprint not found; use a full /Game path for ambiguous names")); }
    UWidgetTree* Tree = Blueprint->WidgetTree;
    if (!Tree) { return LayoutError(TEXT("Widget tree unavailable")); }
    TArray<TSharedPtr<FJsonValue>> Widgets;
    for (const FHostWidget& Entry : HostWidgets(Tree)) { Widgets.Add(MakeShared<FJsonValueObject>(WidgetJson(Entry))); }
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    if (Tree->RootWidget) { Data->SetStringField(TEXT("root"), Tree->RootWidget->GetName()); }
    else { Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>()); }
    Data->SetArrayField(TEXT("widgets"), Widgets);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetToWidget(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid()) { return LayoutError(TEXT("Parameters must be an object")); }
    FString BlueprintName, WidgetName, ClassPath, ParentName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName) || BlueprintName.IsEmpty()) { return LayoutError(TEXT("blueprint_name is required")); }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName)) { return LayoutError(TEXT("widget_name is required")); }
    if (!Params->TryGetStringField(TEXT("widget_class"), ClassPath)) { return LayoutError(TEXT("widget_class must be a /Script class or /Game Widget Blueprint path")); }
    if (Params->HasField(TEXT("parent_name")))
    {
        if (!Params->TryGetStringField(TEXT("parent_name"), ParentName) || ParentName.IsEmpty()) { return LayoutError(TEXT("parent_name must be a nonempty host widget name")); }
    }
    const TSharedPtr<FJsonObject>* LayoutObject = nullptr;
    if (!Params->TryGetObjectField(TEXT("layout"), LayoutObject)) { return LayoutError(TEXT("layout must be an object (use {} for defaults)")); }
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("properties")))
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Params->TryGetObjectField(TEXT("properties"), Object)) { return LayoutError(TEXT("properties must be an object")); }
        Properties = *Object;
    }
    FString Why;
    if (!FUnrealMCPCommonUtils::ValidateAssetCreationPath(TEXT("/Game/Widgets/") + WidgetName, WidgetName, Why)) { return LayoutError(Why); }
    UWidgetBlueprint* Blueprint = FindLayoutWidgetBlueprint(BlueprintName);
    if (!Blueprint) { return LayoutError(TEXT("Widget Blueprint not found; use a full /Game path for ambiguous names")); }
    UWidgetTree* Tree = Blueprint->WidgetTree;
    if (!Tree) { return LayoutError(TEXT("Widget tree unavailable")); }
    if (Blueprint->Status == BS_Error) { return LayoutError(TEXT("Host Blueprint already has compilation errors")); }
    // 检查直接 Outer 下全部对象，孤立控件也不能与新控件重名。
    if (FindObjectFast<UObject>(Tree, FName(*WidgetName))) { return LayoutError(TEXT("Widget name already exists in the host tree, including orphan objects")); }
    UPanelWidget* Parent = nullptr;
    const bool bCreateRoot = Tree->RootWidget == nullptr;
    if (!bCreateRoot)
    {
        for (const FHostWidget& Entry : HostWidgets(Tree))
        {
            const bool bSelected = ParentName.IsEmpty() ? Entry.Widget == Tree->RootWidget : Entry.Widget->GetFName() == FName(*ParentName);
            if (bSelected) { Parent = Cast<UPanelWidget>(Entry.Widget); break; }
        }
        if (!Parent) { return LayoutError(TEXT("Parent must be a panel owned by the host widget tree")); }
        if (!Parent->CanAddMoreChildren()) { return LayoutError(TEXT("Parent panel has no remaining child capacity")); }
    }
    else
    {
        if (!ParentName.IsEmpty()) { return LayoutError(TEXT("parent_name cannot be used when the host tree has no root")); }
        if (WidgetName.Equals(TEXT("RootCanvas"), ESearchCase::IgnoreCase)) { return LayoutError(TEXT("RootCanvas is reserved when creating the default root")); }
        if (FindObjectFast<UObject>(Tree, TEXT("RootCanvas"))) { return LayoutError(TEXT("Cannot create default root: RootCanvas name is already owned")); }
    }
    FLayout Layout;
    const ELayoutKind Kind = bCreateRoot ? ELayoutKind::Canvas : LayoutKind(Parent);
    if (!ParseLayout(*LayoutObject, Kind, Layout, Why)) { return LayoutError(Why); }
    UClass* Class = ResolveClass(ClassPath);
    if (!Class) { return LayoutError(TEXT("Widget class could not be loaded")); }
    if (!Class->IsChildOf(UWidget::StaticClass())) { return LayoutError(TEXT("widget_class must derive from UWidget")); }
    if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { return LayoutError(TEXT("widget_class must be a concrete current UWidget class")); }
    TSet<UClass*> Active, Done;
    if (!CheckDependencies(Class, Blueprint, Active, Done, Why)) { return LayoutError(Why); }
    for (const auto& Property : Properties->Values)
    {
        if (!ValidProperty(Class, Property.Key, Property.Value, Why)) { return LayoutError(Why); }
    }
    // 在独立临时对象上预检值，不触发 UserWidget 初始化，不改动宿主树或类默认对象。
    if (Properties->Values.Num() > 0)
    {
        TStrongObjectPtr<UWidget> Probe(NewObject<UWidget>(GetTransientPackage(), Class, NAME_None, RF_Transient));
        if (!Probe.Get()) { return LayoutError(TEXT("Failed to create transient property validation object")); }
        for (const auto& Property : Properties->Values)
        {
            if (!SetWidgetValue(Probe.Get(), Property.Key, Property.Value, Why)) { return LayoutError(Why); }
        }
    }
    const bool bWasDirty = Blueprint->GetOutermost()->IsDirty();
    const EBlueprintStatus OldStatus = Blueprint->Status;
    TStrongObjectPtr<UWidgetTree> OriginalTree(Tree);
    TStrongObjectPtr<UWidgetBlueprint> BlueprintGuard(Blueprint);
    TStrongObjectPtr<UWidget> NewWidget(nullptr);
    TStrongObjectPtr<UCanvasPanel> NewRoot(nullptr);
    TStrongObjectPtr<UPanelWidget> ParentGuard(Parent);
    FLayout BorderBefore;
    if (UBorder* Border = Cast<UBorder>(Parent))
    {
        BorderBefore.Padding = ReadStruct<FMargin>(Border, TEXT("Padding"), FMargin(0));
        BorderBefore.HAlign = static_cast<EHorizontalAlignment>(ReadByte(Border, TEXT("HorizontalAlignment")));
        BorderBefore.VAlign = static_cast<EVerticalAlignment>(ReadByte(Border, TEXT("VerticalAlignment")));
    }
    bool bCompileAttempted = false;
    auto Rollback = [&](const FString& Message)
    {
        bool bClean = true;
        // 编译可能重实例化对象，优先在当前宿主源树中按本次保留的唯一名称重新定位。
        UWidgetTree* CurrentTree = Blueprint->WidgetTree;
        if (CurrentTree)
        {
            UWidget* Current = FindObjectFast<UWidget>(CurrentTree, FName(*WidgetName));
            if (Current) { bClean = RemoveNewWidget(CurrentTree, Current); }
            if (bCreateRoot)
            {
                UWidget* Root = FindObjectFast<UWidget>(CurrentTree, TEXT("RootCanvas"));
                if (Root) { bClean = RemoveNewWidget(CurrentTree, Root) && bClean; }
            }
            if (!ParentName.IsEmpty() || !bCreateRoot)
            {
                UPanelWidget* RestoredParent = nullptr;
                for (const FHostWidget& Entry : HostWidgets(CurrentTree))
                {
                    if (Entry.Widget->GetFName() == ParentGuard->GetFName()) { RestoredParent = Cast<UPanelWidget>(Entry.Widget); break; }
                }
                if (UBorder* Border = Cast<UBorder>(RestoredParent))
                {
                    Border->SetPadding(BorderBefore.Padding);
                    Border->SetHorizontalAlignment(BorderBefore.HAlign);
                    Border->SetVerticalAlignment(BorderBefore.VAlign);
                }
            }
        }
        else { bClean = false; }
        if (NewWidget.Get())
        {
            if (NewWidget->GetOuter() == OriginalTree.Get()) { bClean = RemoveNewWidget(OriginalTree.Get(), NewWidget.Get()) && bClean; }
        }
        if (NewRoot.Get())
        {
            if (NewRoot->GetOuter() == OriginalTree.Get()) { bClean = RemoveNewWidget(OriginalTree.Get(), NewRoot.Get()) && bClean; }
        }
        bool bRestoreCompile = true;
        if (bCompileAttempted)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            bRestoreCompile = Blueprint->Status != BS_Error;
        }
        else { Blueprint->Status = OldStatus; }
        const bool bRestored = bClean && bRestoreCompile;
        if (bRestored) { Blueprint->GetOutermost()->SetDirtyFlag(bWasDirty); }
        auto Response = LayoutError(Message);
        Response->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
        Response->SetBoolField(TEXT("saved"), false);
        Response->SetBoolField(TEXT("rolled_back"), bRestored);
        Response->SetBoolField(TEXT("rollback_compile_succeeded"), bRestoreCompile);
        return Response;
    };
    // 与 UWidgetBlueprint::PostLoad 一致：先清除源树上的模板标记再构造控件。
    // CreateBlueprint 新建的蓝图不经过 PostLoad，残留的 RF_ArchetypeObject
    // 会沿 Outer 链传播，让本会话内新建的子控件被 IsTemplate 判定为模板，
    // 初始化时触发 InitializeWidgetStatic 错误。
    Tree->ClearFlags(RF_ArchetypeObject);
    if (bCreateRoot)
    {
        NewRoot.Reset(Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas")));
        if (!NewRoot.Get()) { return Rollback(TEXT("Failed to construct default Canvas root")); }
        Tree->RootWidget = NewRoot.Get();
        Parent = NewRoot.Get();
    }
    NewWidget.Reset(Tree->ConstructWidget<UWidget>(Class, FName(*WidgetName)));
    if (!NewWidget.Get()) { return Rollback(TEXT("Failed to construct widget")); }

    for (const auto& Property : Properties->Values)
    {
        if (!SetWidgetValue(NewWidget.Get(), Property.Key, Property.Value, Why)) { return Rollback(Why); }
    }
    UPanelSlot* Slot = Parent->AddChild(NewWidget.Get());
    if (!Slot) { return Rollback(TEXT("Failed to attach widget to parent")); }
    if (!ApplyLayout(Slot, Layout, *LayoutObject)) { return Rollback(TEXT("Parent returned an unsupported slot type")); }
    bCompileAttempted = true;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error) { return Rollback(TEXT("Widget Blueprint compilation failed; no save was attempted")); }
    // 保存失败保留已编译改动并明确报告，避免声称已经写入磁盘。
    if (!UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false))
    {
        auto Response = LayoutError(TEXT("Widget was added and compiled, but saving failed; changes remain in memory"));
        Response->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
        Response->SetStringField(TEXT("widget_name"), WidgetName);
        Response->SetBoolField(TEXT("applied"), true);
        Response->SetBoolField(TEXT("saved"), false);
        return Response;
    }
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("widget_class"), Class->GetPathName());
    Data->SetStringField(TEXT("parent_name"), Parent->GetName());
    Data->SetObjectField(TEXT("layout"), SlotJson(NewWidget.Get()));
    Data->SetBoolField(TEXT("created_root"), bCreateRoot);
    Data->SetBoolField(TEXT("compiled"), true);
    Data->SetBoolField(TEXT("saved"), true);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetWidgetLayout(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid()) { return LayoutError(TEXT("Parameters must be an object")); }
    FString BlueprintName, WidgetName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName) || BlueprintName.IsEmpty()) { return LayoutError(TEXT("blueprint_name is required")); }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty()) { return LayoutError(TEXT("widget_name is required")); }
    const TSharedPtr<FJsonObject>* LayoutObject = nullptr;
    if (!Params->TryGetObjectField(TEXT("layout"), LayoutObject)) { return LayoutError(TEXT("layout must be an object")); }

    UWidgetBlueprint* Blueprint = FindLayoutWidgetBlueprint(BlueprintName);
    if (!Blueprint) { return LayoutError(TEXT("Widget Blueprint not found; use a full /Game path for ambiguous names")); }
    UWidgetTree* Tree = Blueprint->WidgetTree;
    if (!Tree) { return LayoutError(TEXT("Widget tree unavailable")); }
    if (Blueprint->Status == BS_Error) { return LayoutError(TEXT("Host Blueprint already has compilation errors")); }

    UWidget* Target = Tree->FindWidget(FName(*WidgetName));
    if (!Target) { return LayoutError(TEXT("Widget not found in the host tree")); }
    UPanelSlot* Slot = Target->Slot;
    if (!Slot) { return LayoutError(TEXT("Widget has no slot (it may be the root)")); }
    const ELayoutKind Kind = LayoutKind(Slot->Parent);
    if (Kind == ELayoutKind::Unsupported) { return LayoutError(TEXT("Unsupported parent panel type")); }

    FLayout Layout;
    FString Why;
    if (!ParseLayout(*LayoutObject, Kind, Layout, Why)) { return LayoutError(Why); }
    if (!ApplyLayout(Slot, Layout, *LayoutObject)) { return LayoutError(TEXT("Parent returned an unsupported slot type")); }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (Blueprint->Status == BS_Error) { return LayoutError(TEXT("Widget Blueprint compilation failed; no save was attempted")); }
    if (!UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false))
    {
        auto Response = LayoutError(TEXT("Layout was applied and compiled, but saving failed; changes remain in memory"));
        Response->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
        Response->SetStringField(TEXT("widget_name"), WidgetName);
        Response->SetBoolField(TEXT("saved"), false);
        return Response;
    }
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetObjectField(TEXT("layout"), SlotJson(Target));
    Data->SetBoolField(TEXT("compiled"), true);
    Data->SetBoolField(TEXT("saved"), true);
    return FUnrealMCPCommonUtils::CreateSuccessResponse(Data);
}
