#include "MCPServerRunnable.h"
#include "MCPMessageFramer.h"
#include "UnrealMCPBridge.h"
#include "SocketSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "HAL/PlatformTime.h"

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge), ListenerSocket(InListenerSocket), bRunning(true)
{
}

FMCPServerRunnable::~FMCPServerRunnable() = default;

bool FMCPServerRunnable::Init()
{
    return ListenerSocket.IsValid();
}

uint32 FMCPServerRunnable::Run()
{
    while (bRunning)
    {
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            // socket 只能通过所属子系统销毁，由工作线程持有至连接结束。
            ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            TSharedPtr<FSocket> Client = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")),
                [Subsystem](FSocket* Socket)
                {
                    if (Socket)
                    {
                        Socket->Close();
                        Subsystem->DestroySocket(Socket);
                    }
                });
            if (Client.IsValid())
            {
                Client->SetNoDelay(true);
                HandleClientConnection(Client);
            }
        }
        FPlatformProcess::Sleep(0.01f);
    }
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

void FMCPServerRunnable::HandleClientConnection(TSharedPtr<FSocket> Client)
{
    if (!Client->SetNonBlocking(true))
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to enable nonblocking client I/O"));
        return;
    }
    uint8 Buffer[8192];
    FMCPMessageFramer Framer;
    double LastActivity = FPlatformTime::Seconds();
    while (bRunning)
    {
        if (FPlatformTime::Seconds() - LastActivity > 30.0)
        {
            return;
        }
        int32 BytesRead = 0;
        if (!Client->Recv(Buffer, sizeof(Buffer), BytesRead))
        {
            // UE4 将非阻塞 would-block 转为成功且零字节，false 表示断连或读取失败。
            return;
        }
        if (BytesRead <= 0)
        {
            // 暂时无数据不是 EOF，短暂休眠后继续接收且不刷新空闲期限。
            FPlatformProcess::Sleep(0.01f);
            continue;
        }
        LastActivity = FPlatformTime::Seconds();
        for (int32 Index = 0; Index < BytesRead; ++Index)
        {
            FString Message;
            if (!Framer.Push(Buffer[Index], Message))
            {
                SendResponse(Client, TEXT("{\"status\":\"error\",\"error\":\"Invalid or oversized JSON frame\"}"));
                return;
            }
            if (!Message.IsEmpty())
            {
                if (!ProcessMessage(Client, Message))
                {
                    return;
                }
                LastActivity = FPlatformTime::Seconds();
            }
        }
    }
}

bool FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    TSharedPtr<FJsonObject> JsonMessage;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
    {
        return SendResponse(Client, TEXT("{\"status\":\"error\",\"error\":\"Invalid JSON object\"}"));
    }
    FString CommandType;
    // 保留现有 type 优先级，同时接通原先未使用的 command 协议。
    if (!JsonMessage->TryGetStringField(TEXT("type"), CommandType))
    {
        JsonMessage->TryGetStringField(TEXT("command"), CommandType);
    }
    if (CommandType.IsEmpty())
    {
        return SendResponse(Client, TEXT("{\"status\":\"error\",\"error\":\"Missing command type\"}"));
    }
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    if (JsonMessage->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
        if (!JsonMessage->TryGetObjectField(TEXT("params"), ParamsObject))
        {
            return SendResponse(Client, TEXT("{\"status\":\"error\",\"error\":\"params must be an object\"}"));
        }
        Params = *ParamsObject;
    }
    return SendResponse(Client, Bridge->ExecuteCommand(CommandType, Params));
}

bool FMCPServerRunnable::SendResponse(TSharedPtr<FSocket> Client, const FString& Response)
{
    const FTCHARToUTF8 Utf8(*(Response + TEXT("\n")));
    int32 Offset = 0;
    const double Deadline = FPlatformTime::Seconds() + 30.0;
    while (bRunning && Offset < Utf8.Length())
    {
        int32 BytesSent = 0;
        if (Client->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + Offset, Utf8.Length() - Offset, BytesSent))
        {
            Offset += BytesSent;
        }
        else
        {
            const ESocketErrors Error = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            if (Error != SE_EWOULDBLOCK && Error != SE_EINTR)
            {
                return false;
            }
        }
        if (FPlatformTime::Seconds() >= Deadline)
        {
            return false;
        }
        if (BytesSent <= 0)
        {
            FPlatformProcess::Sleep(0.01f);
        }
    }
    return Offset == Utf8.Length();
}
