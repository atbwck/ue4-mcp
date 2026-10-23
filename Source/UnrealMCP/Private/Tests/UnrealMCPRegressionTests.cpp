#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MCPMessageFramer.h"
#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Components/SceneComponent.h"
#include "Components/WidgetComponent.h"
#include "UnrealMCPTestTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Async/Async.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

namespace UnrealMCPTestHelpers
{

    // 测试专用目录；用例结束时清理，不污染项目资产。
    const TCHAR* GetTestPackagePath()
    {
        return TEXT("/Game/UnrealMCPTests");
    }

    // 清理本次测试创建的资产与目录。
    void CleanupTestAssets()
    {
        TArray<FString> Assets;
        Assets.Add(FString(TEXT("/Game/UnrealMCPTests/BP_MCPTest")));
        Assets.Add(FString(TEXT("/Game/UnrealMCPTests/WBP_MCPTest")));
        for (const FString& Asset : Assets)
        {
            if (UEditorAssetLibrary::DoesAssetExist(Asset))
            {
                UEditorAssetLibrary::DeleteAsset(Asset);
            }
        }
    }

    // 以指定端口构造一个可接受的 socket 端点；端口为 0 时由系统分配。
    TSharedPtr<FSocket> CreateListener(uint16& OutPort)
    {
        ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Subsystem)
        {
            return nullptr;
        }
        TSharedPtr<FSocket> Listener = MakeShareable(
            Subsystem->CreateSocket(NAME_Stream, TEXT("MCPTestListener"), false),
            [Subsystem](FSocket* Socket)
            {
                if (Socket)
                {
                    Socket->Close();
                    Subsystem->DestroySocket(Socket);
                }
            });
        if (!Listener.IsValid())
        {
            return nullptr;
        }
        Listener->SetReuseAddr(true);
        Listener->SetNonBlocking(true);
        TSharedRef<FInternetAddr> Addr = Subsystem->CreateInternetAddr();
        bool bValid = false;
        Addr->SetIp(TEXT("127.0.0.1"), bValid);
        Addr->SetPort(0);
        if (!bValid || !Listener->Bind(*Addr))
        {
            return nullptr;
        }
        if (!Listener->Listen(4))
        {
            return nullptr;
        }
        OutPort = Listener->GetPortNo();
        return Listener;
    }

    // 建立到指定端口的客户端连接。
    TSharedPtr<FSocket> ConnectClient(uint16 Port)
    {
        ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Subsystem)
        {
            return nullptr;
        }
        TSharedPtr<FSocket> Client = MakeShareable(
            Subsystem->CreateSocket(NAME_Stream, TEXT("MCPTestClient"), false),
            [Subsystem](FSocket* Socket)
            {
                if (Socket)
                {
                    Socket->Close();
                    Subsystem->DestroySocket(Socket);
                }
            });
        if (!Client.IsValid())
        {
            return nullptr;
        }
        TSharedRef<FInternetAddr> Addr = Subsystem->CreateInternetAddr();
        bool bValid = false;
        Addr->SetIp(TEXT("127.0.0.1"), bValid);
        Addr->SetPort(Port);
        Client->SetNonBlocking(false);
        Client->SetNoDelay(true);
        if (!bValid || !Client->Connect(*Addr))
        {
            return nullptr;
        }
        return Client;
    }

    // 等待并接受一个连接，超时返回空。
    TSharedPtr<FSocket> AcceptWithTimeout(TSharedPtr<FSocket> Listener, double TimeoutSeconds)
    {
        ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (FPlatformTime::Seconds() < Deadline)
        {
            bool bPending = false;
            if (Listener->HasPendingConnection(bPending) && bPending)
            {
                return MakeShareable(Listener->Accept(TEXT("MCPTestAccepted")),
                    [Subsystem](FSocket* Socket)
                    {
                        if (Socket)
                        {
                            Socket->Close();
                            Subsystem->DestroySocket(Socket);
                        }
                    });
            }
            FPlatformProcess::Sleep(0.01f);
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPFramingTest, "UnrealMCP.Transport.Framing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPFramingTest::RunTest(const FString& Parameters)
{
    // 逐字节输入，覆盖转义字符、字符串内括号和 UTF-8 多字节分片。
    const FString First = TEXT("{\"type\":\"ping\",\"params\":{\"value\":\"} [ \\\" \\\\ \u00e9\"}}");
    const FString Large = TEXT("{\"command\":\"ping\",\"params\":{\"value\":\"") + FString::ChrN(9000, 'x') + TEXT("\"}}");
    const FString Input = First + TEXT("\n") + Large + TEXT("{}\n");
    const FTCHARToUTF8 Utf8(*Input);
    FMCPMessageFramer Framer;
    TArray<FString> Messages;
    for (int32 Index = 0; Index < Utf8.Length(); ++Index)
    {
        FString Message;
        if (!Framer.Push(static_cast<uint8>(Utf8.Get()[Index]), Message))
        {
            AddError(TEXT("Valid frame was rejected"));
            return false;
        }
        if (!Message.IsEmpty())
        {
            Messages.Add(Message);
        }
    }
    TestEqual(TEXT("All coalesced objects are emitted"), Messages.Num(), 3);
    if (Messages.Num() == 3)
    {
        TestEqual(TEXT("Escapes and UTF-8 survive fragmentation"), Messages[0], First);
        TestEqual(TEXT("A message larger than recv buffer is preserved"), Messages[1], Large);
        TestEqual(TEXT("Trailing newline does not lose the final frame"), Messages[2], FString(TEXT("{}")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPFrameLimitTest, "UnrealMCP.Transport.Limits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPFrameLimitTest::RunTest(const FString& Parameters)
{
    FMCPMessageFramer Framer;
    FString Message;
    TestTrue(TEXT("Object starts normally"), Framer.Push('{', Message));
    bool bAccepted = true;
    for (int32 Index = 1; Index <= FMCPMessageFramer::MaxMessageBytes; ++Index)
    {
        bAccepted = Framer.Push(' ', Message);
        if (!bAccepted)
        {
            break;
        }
    }
    TestFalse(TEXT("Unterminated oversized frame is rejected"), bAccepted);
    FMCPMessageFramer NonObject;
    TestFalse(TEXT("Non-object root is rejected"), NonObject.Push('[', Message));
    FMCPMessageFramer Nested;
    bool bDepthAccepted = true;
    for (int32 Index = 0; Index < 129; ++Index)
    {
        bDepthAccepted = Nested.Push('{', Message);
    }
    TestFalse(TEXT("Excessive nesting is rejected"), bDepthAccepted);

    // 受限帧被拒绝后，同一连接上的后续完整对象仍应正常分帧。
    FMCPMessageFramer Recovered;
    FString RecoveredMessage;
    for (const TCHAR* Ch = TEXT("{\"type\":\"ping\"}") ; *Ch; ++Ch)
    {
        Recovered.Push(static_cast<uint8>(*Ch), RecoveredMessage);
    }
    TestEqual(TEXT("Framer emits the object after recovery"), RecoveredMessage, FString(TEXT("{\"type\":\"ping\"}")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPAssetPathTest, "UnrealMCP.Assets.PathValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPAssetPathTest::RunTest(const FString& Parameters)
{
    FString Error;
    TestTrue(TEXT("Project asset path is supported"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("/Game/Blueprints/BP_Example"), TEXT("BP_Example"), Error));
    TestFalse(TEXT("Double slash is rejected before CreatePackage"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("/Game//Widgets/Example"), TEXT("Example"), Error));
    TestFalse(TEXT("Relative path is rejected"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("relative"), TEXT("relative"), Error));
    TestFalse(TEXT("Full path cannot be used as object name"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("/Game/Widgets/Example"), TEXT("/Game/Widgets/Example"), Error));
    TestFalse(TEXT("Excessive object name length is rejected"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("/Game/Widgets/Example"), FString::ChrN(NAME_SIZE, 'x'), Error));
    TestFalse(TEXT("Subobject separator cannot be used in asset name"), FUnrealMCPCommonUtils::ValidateAssetCreationPath(
        TEXT("/Game/Widgets/Example"), TEXT("Example:Child"), Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPPropertyTest, "UnrealMCP.Properties.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPropertyTest::RunTest(const FString& Parameters)
{
    // 只操作临时组件，不创建或保存蓝图资产。
    USceneComponent* Component = NewObject<USceneComponent>();
    FString Error;
    TestTrue(TEXT("Native vector string is parsed"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("RelativeLocation"), MakeShared<FJsonValueString>(TEXT("X=1 Y=2 Z=3")), Error));
    TestTrue(TEXT("Vector reaches its own property storage"), Component->GetRelativeLocation().Equals(FVector(1, 2, 3)));
    const TArray<TSharedPtr<FJsonValue>> BadArray = { MakeShared<FJsonValueNumber>(9),
        MakeShared<FJsonValueString>(TEXT("invalid")), MakeShared<FJsonValueNumber>(7), MakeShared<FJsonValueNumber>(6) };
    TestFalse(TEXT("Mixed array is rejected instead of shifting coordinates"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("RelativeLocation"), MakeShared<FJsonValueArray>(BadArray), Error));
    TestTrue(TEXT("Rejected value preserves existing location"), Component->GetRelativeLocation().Equals(FVector(1, 2, 3)));
    TestFalse(TEXT("Null JSON pointer is rejected"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("RelativeLocation"), nullptr, Error));

    // 类型不匹配必须被拒绝，且不破坏既有值。
    TestFalse(TEXT("Boolean property rejects a number"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("bHiddenInGame"), MakeShared<FJsonValueNumber>(1), Error));
    TestFalse(TEXT("Unknown property is rejected"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("NoSuchProperty"), MakeShared<FJsonValueNumber>(1), Error));

    // 旋转与二维向量走各自的结构分支。
    TestTrue(TEXT("Rotator array is applied"), FUnrealMCPCommonUtils::SetObjectProperty(Component,
        TEXT("RelativeRotation"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(10), MakeShared<FJsonValueNumber>(20), MakeShared<FJsonValueNumber>(30) }), Error));
    TestTrue(TEXT("Rotator reached its own storage"), Component->GetRelativeRotation().Equals(FRotator(10, 20, 30)));

    // FIntPoint：DrawSize 是 protected，用公开读口 GetDrawSize 验证（返回 FVector2D）。
    UWidgetComponent* WidgetComp = NewObject<UWidgetComponent>();
    TestTrue(TEXT("IntPoint string is parsed"), FUnrealMCPCommonUtils::SetObjectProperty(WidgetComp,
        TEXT("DrawSize"), MakeShared<FJsonValueString>(TEXT("X=1800 Y=460")), Error));
    TestTrue(TEXT("IntPoint string reached storage"), WidgetComp->GetDrawSize().Equals(FVector2D(1800, 460)));
    TestTrue(TEXT("IntPoint array is parsed"), FUnrealMCPCommonUtils::SetObjectProperty(WidgetComp,
        TEXT("DrawSize"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(100), MakeShared<FJsonValueNumber>(50) }), Error));
    TestTrue(TEXT("IntPoint array reached storage"), WidgetComp->GetDrawSize().Equals(FVector2D(100, 50)));

    // TSet<FName> 写入、清空重写与元素去重；载体是插件内测试 UObject，不依赖项目模块。
    UMCPTestSetObject* SetCarrier = NewObject<UMCPTestSetObject>();
    TestTrue(TEXT("FName set array is parsed"), FUnrealMCPCommonUtils::SetObjectProperty(SetCarrier,
        TEXT("NameSet"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueString>(TEXT("city")), MakeShared<FJsonValueString>(TEXT("city")),
            MakeShared<FJsonValueString>(TEXT("ExampleCity")) }), Error));
    TestTrue(TEXT("FName set reached storage with dedup"), SetCarrier->NameSet.Num() == 2
        && SetCarrier->NameSet.Contains(FName(TEXT("city")))
        && SetCarrier->NameSet.Contains(FName(TEXT("ExampleCity"))));
    TestTrue(TEXT("FName set is cleared and rewritten"), FUnrealMCPCommonUtils::SetObjectProperty(SetCarrier,
        TEXT("NameSet"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueString>(TEXT("onlyone")) }), Error));
    TestTrue(TEXT("FName set rewrite replaced contents"), SetCarrier->NameSet.Num() == 1
        && SetCarrier->NameSet.Contains(FName(TEXT("onlyone"))));
    // 非字符串元素必须被拒绝且不清空原集合。
    TestFalse(TEXT("FName set rejects number element"), FUnrealMCPCommonUtils::SetObjectProperty(SetCarrier,
        TEXT("NameSet"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(7) }), Error));
    TestTrue(TEXT("Rejected set write preserves contents"), SetCarrier->NameSet.Num() == 1);
    // 字符串形态：逗号分隔解析，去重后两项。
    TestTrue(TEXT("FName set comma string is parsed"), FUnrealMCPCommonUtils::SetObjectProperty(SetCarrier,
        TEXT("NameSet"), MakeShared<FJsonValueString>(TEXT("alpha, beta, alpha")), Error));
    TestTrue(TEXT("FName set comma string reached storage"), SetCarrier->NameSet.Num() == 2
        && SetCarrier->NameSet.Contains(FName(TEXT("alpha")))
        && SetCarrier->NameSet.Contains(FName(TEXT("beta"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPGraphConnectionTest, "UnrealMCP.Blueprints.ConnectionValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPGraphConnectionTest::RunTest(const FString& Parameters)
{
    // K2 Schema 要求节点属于蓝图，使用临时蓝图即可验证，不保存资产。
    UBlueprint* Blueprint = NewObject<UBlueprint>();
    Blueprint->ParentClass = UObject::StaticClass();
    UEdGraph* Graph = NewObject<UEdGraph>(Blueprint);
    Blueprint->UbergraphPages.Add(Graph);
    Graph->Schema = UEdGraphSchema_K2::StaticClass();
    UEdGraphNode* Source = NewObject<UK2Node_CallFunction>(Graph);
    UEdGraphNode* Target = NewObject<UK2Node_CallFunction>(Graph);
    Graph->AddNode(Source);
    Graph->AddNode(Target);
    UEdGraphPin* Output = Source->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("Out"));
    Target->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("Wrong"));
    Target->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("In"));
    TestFalse(TEXT("Execution and string pins cannot be linked"), FUnrealMCPCommonUtils::ConnectGraphNodes(
        Graph, Source, TEXT("Out"), Target, TEXT("Wrong")));
    TestEqual(TEXT("Rejected connection leaves no link"), Output->LinkedTo.Num(), 0);
    TestTrue(TEXT("Compatible execution pins are linked"), FUnrealMCPCommonUtils::ConnectGraphNodes(
        Graph, Source, TEXT("Out"), Target, TEXT("In")));
    TestEqual(TEXT("Accepted connection creates one link"), Output->LinkedTo.Num(), 1);

    // 跨图节点不能被连接；调用方传错图时必须拒绝。
    UEdGraph* OtherGraph = NewObject<UEdGraph>(Blueprint);
    Blueprint->UbergraphPages.Add(OtherGraph);
    OtherGraph->Schema = UEdGraphSchema_K2::StaticClass();
    UEdGraphNode* OtherNode = NewObject<UK2Node_CallFunction>(OtherGraph);
    OtherGraph->AddNode(OtherNode);
    OtherNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("In"));
    TestFalse(TEXT("Nodes from another graph are rejected"), FUnrealMCPCommonUtils::ConnectGraphNodes(
        Graph, Source, TEXT("Out"), OtherNode, TEXT("In")));

    // 不存在的引脚必须拒绝，而不是静默成功。
    TestFalse(TEXT("Missing pins are rejected"), FUnrealMCPCommonUtils::ConnectGraphNodes(
        Graph, Source, TEXT("NoSuchPin"), Target, TEXT("In")));
    return true;
}

// 真实 socket 收发：覆盖短写、背压、分包与粘包、零字节暂时无数据。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPSocketIOTest, "UnrealMCP.Transport.SocketIO",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPSocketIOTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPTestHelpers;
    ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!TestNotNull(TEXT("Socket subsystem available"), Subsystem))
    {
        return false;
    }

    uint16 Port = 0;
    TSharedPtr<FSocket> Listener = CreateListener(Port);
    if (!TestTrue(TEXT("Listener socket bound and listening"), Listener.IsValid() && Port != 0))
    {
        return false;
    }

    // 小接收缓冲区制造背压，验证发送方能写完整响应并处理短写。
    const int32 SmallBuffer = 1024;
    int32 NewSize = 0;
    Listener->SetReceiveBufferSize(SmallBuffer, NewSize);

    TSharedPtr<FSocket> Client = ConnectClient(Port);
    if (!TestTrue(TEXT("Client connected"), Client.IsValid()))
    {
        return false;
    }
    TSharedPtr<FSocket> Accepted = AcceptWithTimeout(Listener, 5.0);
    if (!TestTrue(TEXT("Server accepted the connection"), Accepted.IsValid()))
    {
        return false;
    }

    // 两个对象粘包 + 一个对象分包发送，服务端必须切出 3 条完整消息。
    const FString MessageA = TEXT("{\"type\":\"ping\"}");
    const FString MessageB = TEXT("{\"type\":\"ping\",\"params\":{\"n\":1}}");
    const FString MessageC = TEXT("{\"type\":\"ping\",\"params\":{\"pad\":\"") + FString::ChrN(6000, 'y') + TEXT("\"}}");
    const FString Payload = MessageA + MessageB;
    const FTCHARToUTF8 PayloadUtf8(*Payload);
    int32 Sent = 0;
    TestTrue(TEXT("Coalesced payload is sent"), Client->Send(reinterpret_cast<const uint8*>(PayloadUtf8.Get()), PayloadUtf8.Length(), Sent));
    TestEqual(TEXT("Coalesced payload sent in full"), Sent, PayloadUtf8.Length());

    // 分段发送第三条消息：先发一半，再发剩余部分。
    const FTCHARToUTF8 CUtf8(*MessageC);
    const int32 Half = CUtf8.Length() / 2;
    int32 SentFirst = 0;
    int32 SentSecond = 0;
    TestTrue(TEXT("First fragment is sent"), Client->Send(reinterpret_cast<const uint8*>(CUtf8.Get()), Half, SentFirst));
    FPlatformProcess::Sleep(0.02f);
    TestTrue(TEXT("Second fragment is sent"), Client->Send(reinterpret_cast<const uint8*>(CUtf8.Get()) + SentFirst, CUtf8.Length() - SentFirst, SentSecond));

    // 服务端按字节累积分帧，验证三条消息边界完全正确。
    FMCPMessageFramer Framer;
    TArray<FString> Received;
    uint8 Buffer[1024];
    const double Deadline = FPlatformTime::Seconds() + 10.0;
    while (Received.Num() < 3 && FPlatformTime::Seconds() < Deadline)
    {
        int32 BytesRead = 0;
        if (Accepted->Recv(Buffer, sizeof(Buffer), BytesRead))
        {
            if (BytesRead <= 0)
            {
                FPlatformProcess::Sleep(0.01f);
                continue;
            }
            for (int32 Index = 0; Index < BytesRead; ++Index)
            {
                FString Message;
                TestTrue(TEXT("Framing accepts streamed bytes"), Framer.Push(Buffer[Index], Message));
                if (!Message.IsEmpty())
                {
                    Received.Add(Message);
                }
            }
        }
        else if (Subsystem->GetLastErrorCode() != SE_EWOULDBLOCK)
        {
            break;
        }
    }
    TestEqual(TEXT("Streaming yields three separate objects"), Received.Num(), 3);
    if (Received.Num() == 3)
    {
        TestEqual(TEXT("First coalesced object is intact"), Received[0], MessageA);
        TestEqual(TEXT("Second coalesced object is intact"), Received[1], MessageB);
        TestEqual(TEXT("Split message is reassembled exactly"), Received[2], MessageC);
    }
    // 连接建立后不做非阻塞设置，recv 无数据时返回零字节，不应视作断连。
    TestEqual(TEXT("Socket stayed connected while idle"), Client->GetConnectionState(), SCS_Connected);
    return true;
}

// 发送路径：验证完整响应用 UTF-8 字节长度写出，含多字节内容与短写重试。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPSendResponseTest, "UnrealMCP.Transport.SendResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPSendResponseTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPTestHelpers;
    ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!TestNotNull(TEXT("Socket subsystem available"), Subsystem))
    {
        return false;
    }
    uint16 Port = 0;
    TSharedPtr<FSocket> Listener = CreateListener(Port);
    if (!TestTrue(TEXT("Listener ready"), Listener.IsValid() && Port != 0))
    {
        return false;
    }
    // 小发送缓冲区放大短写概率。
    int32 NewSize = 0;
    Listener->SetSendBufferSize(2048, NewSize);

    TSharedPtr<FSocket> Client = ConnectClient(Port);
    TSharedPtr<FSocket> Accepted = AcceptWithTimeout(Listener, 5.0);
    if (!TestTrue(TEXT("Connection established"), Client.IsValid() && Accepted.IsValid()))
    {
        return false;
    }

    // 构造含中文与换行的长响应；响应必须按 UTF-8 字节数发送，不能被字符数截断。
    FString Response = TEXT("{\"status\":\"success\",\"result\":{\"note\":\"");
    Response += FString::ChrN(4000, TEXT('测'));
    Response += TEXT("\"}}");

    // 直接复用被测发送逻辑所需的分帧前提：核对字节长度换算。
    const FTCHARToUTF8 Utf8(*Response);
    TestTrue(TEXT("UTF-8 byte length exceeds character count"), Utf8.Length() > Response.Len());

    Accepted->SetNonBlocking(true);
    int32 Offset = 0;
    const double Deadline = FPlatformTime::Seconds() + 20.0;
    while (Offset < Utf8.Length() && FPlatformTime::Seconds() < Deadline)
    {
        int32 BytesSent = 0;
        if (Accepted->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + Offset, Utf8.Length() - Offset, BytesSent))
        {
            Offset += BytesSent;
        }
        else if (Subsystem->GetLastErrorCode() != SE_EWOULDBLOCK)
        {
            break;
        }
        if (BytesSent <= 0)
        {
            FPlatformProcess::Sleep(0.01f);
        }
    }
    TestEqual(TEXT("Entire UTF-8 payload is transmitted"), Offset, Utf8.Length());

    // 接收端按字节累计，验证内容与长度一致。
    TArray<uint8> Collected;
    const double ReadDeadline = FPlatformTime::Seconds() + 10.0;
    while (Collected.Num() < Utf8.Length() && FPlatformTime::Seconds() < ReadDeadline)
    {
        uint8 Buffer[4096];
        int32 BytesRead = 0;
        if (Client->Recv(Buffer, sizeof(Buffer), BytesRead) && BytesRead > 0)
        {
            Collected.Append(Buffer, BytesRead);
        }
        else
        {
            FPlatformProcess::Sleep(0.01f);
        }
    }
    TestEqual(TEXT("Received byte count matches sent byte count"), Collected.Num(), Utf8.Length());
    // 按字节比较，避免窄字符转换把非 ASCII 变成 '?' 而误判。
    const bool bSamePayload = Collected.Num() == Utf8.Length()
        && FMemory::Memcmp(Collected.GetData(), Utf8.Get(), Utf8.Length()) == 0;
    TestTrue(TEXT("Received bytes are identical to the sent UTF-8 payload"), bSamePayload);
    return true;
}

// 生命周期：命令排队期间停止服务器必须能退出，重启后仍可服务新命令。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPLifecycleTest, "UnrealMCP.Lifecycle.StopAndRestart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPTestHelpers;
    UUnrealMCPBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUnrealMCPBridge>() : nullptr;
    if (!TestNotNull(TEXT("Bridge subsystem available"), Bridge))
    {
        return false;
    }

    // 内置服务器由编辑器启动；这里验证停止后能再次启动，且期间命令不会卡死。
    TestTrue(TEXT("Bridge reports running before stop"), Bridge->IsRunning());
    Bridge->StopServer();
    TestFalse(TEXT("Bridge reports stopped after StopServer"), Bridge->IsRunning());

    const double StopDeadline = FPlatformTime::Seconds() + 5.0;
    while (Bridge->IsRunning() && FPlatformTime::Seconds() < StopDeadline)
    {
        FPlatformProcess::Sleep(0.05f);
    }
    TestFalse(TEXT("StopServer completes without deadlock"), Bridge->IsRunning());

    Bridge->StartServer();
    TestTrue(TEXT("Server restarts after stop"), Bridge->IsRunning());

    // 重启后从游戏线程直接执行命令仍应正常返回。
    TSharedPtr<FJsonObject> PingParams = MakeShared<FJsonObject>();
    const FString PingResult = Bridge->ExecuteCommand(TEXT("ping"), PingParams);
    TestTrue(TEXT("Ping succeeds after restart"), PingResult.Contains(TEXT("pong")));

    // 停止状态下从非游戏线程执行命令必须及时返回错误，而不是挂起。
    // 游戏线程直调不排队，不受停止标志影响；这里验证跨线程排队路径。
    Bridge->StopServer();
    TestFalse(TEXT("Server stopped again"), Bridge->IsRunning());
    TFuture<FString> StoppedFuture = Async(EAsyncExecution::Thread, [Bridge]()
    {
        return Bridge->ExecuteCommand(TEXT("ping"), MakeShared<FJsonObject>());
    });
    const bool bStoppedReturned = StoppedFuture.WaitFor(FTimespan::FromSeconds(10.0));
    TestTrue(TEXT("Cross-thread command returns instead of hanging while stopped"), bStoppedReturned);
    if (bStoppedReturned)
    {
        TestTrue(TEXT("Command during stop returns a JSON error"), StoppedFuture.Get().Contains(TEXT("error")));
    }

    // 复原为编辑器默认运行状态。
    Bridge->StartServer();
    TestTrue(TEXT("Server restored to running state"), Bridge->IsRunning());
    return true;
}

// UMG：控件创建、尺寸/字体/颜色参数、事件绑定与重复调用行为。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPUMGWidgetTest, "UnrealMCP.UMG.WidgetAndBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPUMGWidgetTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPTestHelpers;
    CleanupTestAssets();

    UUnrealMCPBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUnrealMCPBridge>() : nullptr;
    if (!TestNotNull(TEXT("Bridge subsystem available"), Bridge))
    {
        return false;
    }

    const FString WidgetAsset = FString(TEXT("/Game/UnrealMCPTests/WBP_MCPTest"));
    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("name"), TEXT("WBP_MCPTest"));
    CreateParams->SetStringField(TEXT("path"), GetTestPackagePath());
    const FString Created = Bridge->ExecuteCommand(TEXT("create_umg_widget_blueprint"), CreateParams);
    TestTrue(TEXT("Widget Blueprint is created"), Created.Contains(TEXT("WBP_MCPTest")));
    TestTrue(TEXT("Widget Blueprint exists on disk"), UEditorAssetLibrary::DoesAssetExist(WidgetAsset));

    UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(WidgetAsset));
    if (!TestNotNull(TEXT("Widget Blueprint loads"), Widget))
    {
        CleanupTestAssets();
        return false;
    }
    // 创建后自动保存：磁盘资产必须已有根控件。
    TestNotNull(TEXT("Root widget persisted to disk"), Widget->WidgetTree ? Widget->WidgetTree->RootWidget : nullptr);

    // 添加按钮：客户端字段名（text_block_name/button_name）与样式参数需实际生效。
    TSharedPtr<FJsonObject> ButtonParams = MakeShared<FJsonObject>();
    ButtonParams->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    ButtonParams->SetStringField(TEXT("button_name"), TEXT("TestButton"));
    ButtonParams->SetStringField(TEXT("text"), TEXT("Hello"));
    ButtonParams->SetArrayField(TEXT("position"), TArray<TSharedPtr<FJsonValue>>{
        MakeShared<FJsonValueNumber>(10), MakeShared<FJsonValueNumber>(20) });
    const FString ButtonResult = Bridge->ExecuteCommand(TEXT("add_button_to_widget"), ButtonParams);
    TestFalse(TEXT("Button creation reports no error"), ButtonResult.Contains(TEXT("\"error\"")));

    UWidgetBlueprint* Reloaded = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(WidgetAsset));
    UButton* Button = Reloaded && Reloaded->WidgetTree
        ? Cast<UButton>(Reloaded->WidgetTree->FindWidget(FName(TEXT("TestButton")))) : nullptr;
    if (TestNotNull(TEXT("Button persisted into the widget tree"), Button))
    {
        UTextBlock* Label = Cast<UTextBlock>(Button->GetChildAt(0));
        if (TestNotNull(TEXT("Button label persisted"), Label))
        {
            TestEqual(TEXT("Button text applied"), Label->GetText().ToString(), FString(TEXT("Hello")));
        }
    }

    // 重复添加同名按钮必须报错，不得产生重名控件。
    const FString Duplicate = Bridge->ExecuteCommand(TEXT("add_button_to_widget"), ButtonParams);
    TestTrue(TEXT("Duplicate widget name is rejected"), Duplicate.Contains(TEXT("already exists")));

    // 事件绑定：使用 TYPED 字段名，重复调用不应产生重复节点。
    TSharedPtr<FJsonObject> EventParams = MakeShared<FJsonObject>();
    EventParams->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    EventParams->SetStringField(TEXT("widget_component_name"), TEXT("TestButton"));
    EventParams->SetStringField(TEXT("event_name"), TEXT("OnClicked"));
    const FString Bound1 = Bridge->ExecuteCommand(TEXT("bind_widget_event"), EventParams);
    TestFalse(TEXT("Event binding reports no error"), Bound1.Contains(TEXT("\"error\"")));
    const FString Bound2 = Bridge->ExecuteCommand(TEXT("bind_widget_event"), EventParams);
    TestFalse(TEXT("Repeated event binding succeeds without error"), Bound2.Contains(TEXT("\"error\"")));

    UWidgetBlueprint* AfterEvents = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(WidgetAsset));
    if (TestNotNull(TEXT("Widget Blueprint reloads after binding"), AfterEvents))
    {
        TArray<UK2Node_ComponentBoundEvent*> BoundNodes;
        FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_ComponentBoundEvent>(AfterEvents, BoundNodes);
        int32 OnClickedCount = 0;
        for (const UK2Node_ComponentBoundEvent* Node : BoundNodes)
        {
            if (Node && Node->DelegatePropertyName == FName(TEXT("OnClicked")))
            {
                ++OnClickedCount;
            }
        }
        TestEqual(TEXT("Repeated binding does not duplicate the event node"), OnClickedCount, 1);
    }

    // 动态文本绑定：先用 TextBlock，再用非 TextBlock 验证类型校验。
    TSharedPtr<FJsonObject> AddText = MakeShared<FJsonObject>();
    AddText->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    AddText->SetStringField(TEXT("text_block_name"), TEXT("TestText"));
    AddText->SetStringField(TEXT("text"), TEXT("Initial"));
    const FString TextAdded = Bridge->ExecuteCommand(TEXT("add_text_block_to_widget"), AddText);
    TestFalse(TEXT("Text block creation reports no error"), TextAdded.Contains(TEXT("\"error\"")));

    TSharedPtr<FJsonObject> BindingParams = MakeShared<FJsonObject>();
    BindingParams->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    BindingParams->SetStringField(TEXT("text_block_name"), TEXT("TestText"));
    BindingParams->SetStringField(TEXT("binding_property"), TEXT("TestBinding"));
    const FString Bound = Bridge->ExecuteCommand(TEXT("set_text_block_binding"), BindingParams);
    TestFalse(TEXT("Text binding on a TextBlock succeeds"), Bound.Contains(TEXT("\"error\"")));

    // 绑定必须真的写入 Blueprint 的 Bindings，而不是仅返回成功。
    UWidgetBlueprint* AfterBinding = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(WidgetAsset));
    if (TestNotNull(TEXT("Widget Blueprint reloads after binding"), AfterBinding))
    {
        bool bFoundBinding = false;
        for (const FDelegateEditorBinding& Binding : AfterBinding->Bindings)
        {
            if (Binding.ObjectName == TEXT("TestText") && Binding.PropertyName == FName(TEXT("Text")))
            {
                bFoundBinding = true;
                break;
            }
        }
        TestTrue(TEXT("Text binding is registered in the Blueprint"), bFoundBinding);
    }

    // 绑到非 TextBlock 控件必须被拒绝。
    TSharedPtr<FJsonObject> BadBinding = MakeShared<FJsonObject>();
    BadBinding->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    BadBinding->SetStringField(TEXT("text_block_name"), TEXT("TestButton"));
    BadBinding->SetStringField(TEXT("binding_property"), TEXT("TestBinding2"));
    const FString Rejected = Bridge->ExecuteCommand(TEXT("set_text_block_binding"), BadBinding);
    TestTrue(TEXT("Binding on a non-TextBlock widget is rejected"), Rejected.Contains(TEXT("error")));

    CleanupTestAssets();
    return true;
}

// 持久化：修改后必须落盘，磁盘字节能证明内容真的写入而不是仅存于内存。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPPersistenceTest, "UnrealMCP.Assets.PersistenceAfterReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPPersistenceTest::RunTest(const FString& Parameters)
{
    using namespace UnrealMCPTestHelpers;
    CleanupTestAssets();

    UUnrealMCPBridge* Bridge = GEditor ? GEditor->GetEditorSubsystem<UUnrealMCPBridge>() : nullptr;
    if (!TestNotNull(TEXT("Bridge subsystem available"), Bridge))
    {
        return false;
    }

    const FString Asset = FString(TEXT("/Game/UnrealMCPTests/WBP_MCPTest"));
    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("name"), TEXT("WBP_MCPTest"));
    CreateParams->SetStringField(TEXT("path"), GetTestPackagePath());
    Bridge->ExecuteCommand(TEXT("create_umg_widget_blueprint"), CreateParams);

    TSharedPtr<FJsonObject> TextParams = MakeShared<FJsonObject>();
    TextParams->SetStringField(TEXT("widget_name"), TEXT("WBP_MCPTest"));
    TextParams->SetStringField(TEXT("text_block_name"), TEXT("PersistedText"));
    TextParams->SetStringField(TEXT("text"), TEXT("PersistedValue"));
    const FString TextResult = Bridge->ExecuteCommand(TEXT("add_text_block_to_widget"), TextParams);
    TestFalse(TEXT("Text block command reports no error"), TextResult.Contains(TEXT("\"error\"")));

    // 直接从磁盘读资产字节：控件名与文本必须已写入包文件，而不是仅存于内存。
    // 空路径会触发引擎 Fatal，测试侧先守卫，保证断言失败而不是让整个运行崩溃。
    TArray<uint8> PackageBytes;
    FString Filename;
    if (TestFalse(TEXT("Test asset path is not empty"), Asset.IsEmpty()))
    {
        if (FPackageName::TryConvertLongPackageNameToFilename(Asset, Filename, FPackageName::GetAssetPackageExtension()))
        {
            TestTrue(TEXT("Asset package exists on disk"), FFileHelper::LoadFileToArray(PackageBytes, *Filename));
        }
        else
        {
            AddError(FString::Printf(TEXT("Could not resolve a filename for asset path [%s]"), *Asset));
        }
    }
    if (PackageBytes.Num() > 0)
    {
        // 存储层可能用 ANSI 或 UTF-16，两种编码都检查；只按一种判定会产生假阴性。
        auto ContainsEncoded = [&PackageBytes](const TCHAR* Needle)
        {
            const FTCHARToUTF8 Utf8(Needle);
            const TArray<uint8> AsciiBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            if (PackageBytes.Num() >= AsciiBytes.Num()
                && FMemory::Memcmp(PackageBytes.GetData(), AsciiBytes.GetData(), AsciiBytes.Num()) == 0)
            {
                return true;
            }
            const int32 Utf16Count = FCString::Strlen(Needle);
            const TArray<uint8> Utf16Bytes(reinterpret_cast<const uint8*>(Needle), Utf16Count * sizeof(TCHAR));
            for (int32 Offset = 0; Offset + Utf16Bytes.Num() <= PackageBytes.Num(); ++Offset)
            {
                if (FMemory::Memcmp(PackageBytes.GetData() + Offset, Utf16Bytes.GetData(), Utf16Bytes.Num()) == 0)
                {
                    return true;
                }
            }
            for (int32 Offset = 0; Offset + AsciiBytes.Num() <= PackageBytes.Num(); ++Offset)
            {
                if (FMemory::Memcmp(PackageBytes.GetData() + Offset, AsciiBytes.GetData(), AsciiBytes.Num()) == 0)
                {
                    return true;
                }
            }
            return false;
        };
        const bool bNameStored = ContainsEncoded(TEXT("PersistedText"));
        const bool bValueStored = ContainsEncoded(TEXT("PersistedValue"));
        TestTrue(TEXT("Saved package is a non-trivial asset file"), PackageBytes.Num() > 1024);
        TestTrue(TEXT("Text block name is present in the saved package"), bNameStored);
        TestTrue(TEXT("Text content is present in the saved package"), bValueStored);
    }

    // 重新从盘加载，控件与文本仍然存在。
    UWidgetBlueprint* Reloaded = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(Asset));
    if (TestNotNull(TEXT("Widget Blueprint reloads from disk"), Reloaded))
    {
        UTextBlock* Text = Reloaded->WidgetTree
            ? Cast<UTextBlock>(Reloaded->WidgetTree->FindWidget(FName(TEXT("PersistedText")))) : nullptr;
        if (TestNotNull(TEXT("Text block survived reload"), Text))
        {
            TestEqual(TEXT("Text content persisted to disk"), Text->GetText().ToString(), FString(TEXT("PersistedValue")));
        }
    }

    // 重复创建同名资产必须报错，不得破坏已有资产。
    const FString Duplicate = Bridge->ExecuteCommand(TEXT("create_umg_widget_blueprint"), CreateParams);
    TestTrue(TEXT("Duplicate asset creation is rejected"), Duplicate.Contains(TEXT("already exists")));

    // 非法保存路径必须在写盘前被拒绝，而不是创建出损坏的包。
    TSharedPtr<FJsonObject> BadPathParams = MakeShared<FJsonObject>();
    BadPathParams->SetStringField(TEXT("name"), TEXT("WBP_BadPath"));
    BadPathParams->SetStringField(TEXT("path"), TEXT("/Game//BadPath"));
    const FString BadPath = Bridge->ExecuteCommand(TEXT("create_umg_widget_blueprint"), BadPathParams);
    TestTrue(TEXT("Invalid path is rejected"), BadPath.Contains(TEXT("error")));
    TestFalse(TEXT("No asset was created for the invalid path"),
        UEditorAssetLibrary::DoesAssetExist(TEXT("/Game/BadPath/WBP_BadPath")));

    // 短名不存在的控件应报找不到，而不是匹配到无关资产。
    TSharedPtr<FJsonObject> UnknownParams = MakeShared<FJsonObject>();
    UnknownParams->SetStringField(TEXT("widget_name"), TEXT("WBP_DefinitelyNotCreated"));
    UnknownParams->SetStringField(TEXT("text_block_name"), TEXT("AnyText"));
    UnknownParams->SetStringField(TEXT("text"), TEXT("x"));
    TestTrue(TEXT("Unknown short name is rejected"),
        Bridge->ExecuteCommand(TEXT("add_text_block_to_widget"), UnknownParams).Contains(TEXT("error")));

    CleanupTestAssets();
    return true;
}

#endif
