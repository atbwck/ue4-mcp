#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include <exception>

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
    MaterialCommands = MakeShared<FUnrealMCPMaterialCommands>();
    DebugCommands = MakeShared<FUnrealMCPDebugCommands>();
}

UUnrealMCPBridge::~UUnrealMCPBridge() = default;

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Initializing"));

    bIsRunning = false;
    ListenerSocket = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Commandlets run headlessly and may mutate packages; they never need a live
    // TCP MCP listener. Starting the listener during EditorEngine initialization can
    // stall a commandlet before its Main() executes, so keep it editor-session only.
    if (IsRunningCommandlet())
    {
        UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: commandlet mode; TCP server disabled"));
        return;
    }

    // Start the server automatically for normal editor sessions.
    StartServer();
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false),
        [SocketSubsystem](FSocket* Socket)
        {
            if (Socket)
            {
                Socket->Close();
                SocketSubsystem->DestroySocket(Socket);
            }
        });
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    if (!NewListenerSocket->SetNonBlocking(true))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to enable nonblocking listener I/O"));
        return;
    }

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // 每次启动独立创建取消状态，旧队列任务不能跨越服务器重启。
    StopRequested = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);
    ServerRunnable = new FMCPServerRunnable(this, ListenerSocket);
    ServerThread = FRunnableThread::Create(
        ServerRunnable,
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    if (StopRequested.IsValid())
    {
        *StopRequested = true;
    }
    if (ServerRunnable)
    {
        ServerRunnable->Stop();
    }
    if (ServerThread)
    {
        ServerThread->WaitForCompletion();
        delete ServerThread;
        ServerThread = nullptr;
    }
    delete ServerRunnable;
    ServerRunnable = nullptr;
    ListenerSocket.Reset();

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// Execute a command received from a client
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return TEXT("{\"status\":\"error\",\"error\":\"Invalid params\"}");
    }
    if (IsInGameThread())
    {
        return ExecuteCommandOnGameThread(CommandType, Params);
    }
    const auto Cancellation = StopRequested;
    if (!Cancellation.IsValid())
    {
        return TEXT("{\"status\":\"error\",\"error\":\"Server stopped\"}");
    }
    auto Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
    TFuture<FString> Future = Promise->GetFuture();
    const TWeakObjectPtr<UUnrealMCPBridge> WeakBridge(this);
    FString ParamsText;
    FJsonSerializer::Serialize(Params.ToSharedRef(), TJsonWriterFactory<>::Create(&ParamsText));
    // 队列任务持有结果存储和会话状态，停止后不会访问已销毁的桥接对象。
    AsyncTask(ENamedThreads::GameThread, [WeakBridge, CommandType, ParamsText, Promise, Cancellation]()
    {
        if (*Cancellation)
        {
            Promise->SetValue(TEXT("{\"status\":\"error\",\"error\":\"Server stopped\"}"));
            return;
        }
        UUnrealMCPBridge* Bridge = WeakBridge.Get();
        if (!Bridge)
        {
            Promise->SetValue(TEXT("{\"status\":\"error\",\"error\":\"Bridge unavailable\"}"));
            return;
        }
        // JSON 的默认共享指针非线程安全，在游戏线程重建参数避免跨线程引用计数。
        TSharedPtr<FJsonObject> GameThreadParams;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ParamsText), GameThreadParams))
        {
            Promise->SetValue(TEXT("{\"status\":\"error\",\"error\":\"Invalid params\"}"));
            return;
        }
        Promise->SetValue(Bridge->ExecuteCommandOnGameThread(CommandType, GameThreadParams));
    });
    // 游戏线程停止服务器时，工作线程必须能退出而无需等待该线程执行任务。
    while (!Future.WaitFor(FTimespan::FromMilliseconds(10)))
    {
        if (*Cancellation)
        {
            return TEXT("{\"status\":\"error\",\"error\":\"Server stopped\"}");
        }
    }
    return Future.Get();
}

FString UUnrealMCPBridge::ExecuteCommandOnGameThread(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    static const TSet<FString> EditorCommandNames = { TEXT("get_actors_in_level"), TEXT("find_actors_by_name"), TEXT("spawn_actor"), TEXT("create_actor"), TEXT("delete_actor"), TEXT("set_actor_transform"), TEXT("get_actor_properties"), TEXT("set_actor_property"), TEXT("set_world_game_mode"), TEXT("set_actor_skeletal_mesh"), TEXT("spawn_blueprint_actor"), TEXT("focus_viewport"), TEXT("take_screenshot"), TEXT("save_level"), TEXT("new_level"), TEXT("open_level"), TEXT("duplicate_level"), TEXT("start_pie"), TEXT("stop_pie"), TEXT("get_pie_state") };
    static const TSet<FString> BlueprintCommandNames = { TEXT("create_blueprint"), TEXT("add_component_to_blueprint"), TEXT("set_component_property"), TEXT("set_physics_properties"), TEXT("compile_blueprint"), TEXT("set_blueprint_property"), TEXT("set_static_mesh_properties"), TEXT("set_pawn_properties") };
    static const TSet<FString> BlueprintNodeCommandNames = { TEXT("format_blueprint_graph"), TEXT("get_blueprint_graph"), TEXT("connect_blueprint_nodes"), TEXT("add_blueprint_get_self_component_reference"), TEXT("add_blueprint_self_reference"), TEXT("find_blueprint_nodes"), TEXT("add_blueprint_event_node"), TEXT("add_blueprint_input_action_node"), TEXT("add_blueprint_function_node"), TEXT("add_blueprint_get_component_node"), TEXT("add_blueprint_variable"), TEXT("add_blueprint_cast_node"), TEXT("add_blueprint_external_member_get_node"), TEXT("get_blueprint_node_details"), TEXT("delete_blueprint_node"), TEXT("disconnect_blueprint_nodes"), TEXT("set_blueprint_node_property"), TEXT("prune_unused_nodes"), TEXT("list_blueprint_graphs"), TEXT("create_blueprint_graph"), TEXT("set_blueprint_pin_default") };
    static const TSet<FString> ProjectCommandNames = { TEXT("create_input_mapping"), TEXT("create_data_table"), TEXT("add_row"), TEXT("read_row"), TEXT("set_asset_property"), TEXT("refresh_project_reflection"), TEXT("compile_project"), TEXT("save_assets") };
    static const TSet<FString> UMGCommandNames = { TEXT("list_widget_blueprints"), TEXT("get_widget_tree"), TEXT("add_widget_to_widget"), TEXT("set_widget_layout"), TEXT("create_umg_widget_blueprint"), TEXT("add_text_block_to_widget"), TEXT("add_button_to_widget"), TEXT("add_image_to_widget"), TEXT("bind_widget_event"), TEXT("set_text_block_binding"), TEXT("add_widget_to_viewport") };
    static const TSet<FString> MaterialCommandNames = { TEXT("create_material"), TEXT("add_vector_parameter"), TEXT("set_material_on_component"), TEXT("create_dynamic_material_instance"), TEXT("set_material_parameter"), TEXT("get_component_materials"), TEXT("get_material_graph"), TEXT("list_material_parameters"), TEXT("add_material_expression"), TEXT("set_material_expression_property"), TEXT("delete_material_expression"), TEXT("connect_material_expressions"), TEXT("disconnect_material_expressions"), TEXT("connect_material_property"), TEXT("disconnect_material_property"), TEXT("set_material_settings"), TEXT("add_material_comment"), TEXT("format_material_graph"), TEXT("create_material_instance"), TEXT("set_material_instance_parameter"), TEXT("get_material_instance_parameters") };
    static const TSet<FString> DebugCommandNames = { TEXT("get_output_log") };
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);

        try
        {
            TSharedPtr<FJsonObject> ResultJson;

            bool OptionalBool;
            FString OptionalGraph;
            if ((Params->HasField(TEXT("defer_save")) && !Params->TryGetBoolField(TEXT("defer_save"), OptionalBool)) ||
                (Params->HasField(TEXT("pie")) && !Params->TryGetBoolField(TEXT("pie"), OptionalBool)) ||
                (Params->HasField(TEXT("graph_name")) && !Params->TryGetStringField(TEXT("graph_name"), OptionalGraph)))
            {
                ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("defer_save/pie must be booleans and graph_name must be a string"));
            }
            else if (CommandType == TEXT("ping"))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            }
            // Editor Commands (including actor manipulation)
            else if (EditorCommandNames.Contains(CommandType))
            {
                ResultJson = EditorCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Commands
            else if (BlueprintCommandNames.Contains(CommandType))
            {
                ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Node Commands
            else if (BlueprintNodeCommandNames.Contains(CommandType))
            {
                ResultJson = BlueprintNodeCommands->HandleCommand(CommandType, Params);
            }
            // Project Commands
            else if (ProjectCommandNames.Contains(CommandType))
            {
                ResultJson = ProjectCommands->HandleCommand(CommandType, Params);
            }
            // UMG Commands
            else if (UMGCommandNames.Contains(CommandType))
            {
                ResultJson = UMGCommands->HandleCommand(CommandType, Params);
            }
            // Material Commands
            else if (MaterialCommandNames.Contains(CommandType))
            {
                ResultJson = MaterialCommands->HandleCommand(CommandType, Params);
            }
            // Debug Commands
            else if (DebugCommandNames.Contains(CommandType))
            {
                ResultJson = DebugCommands->HandleCommand(CommandType, Params);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));

                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                return ResultString;
            }

            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;

            if (!ResultJson.IsValid())
            {
                ResultJson = FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Command returned no result"));
            }
            if (ResultJson->HasField(TEXT("error")))
            {
                bSuccess = false;
                ErrorMessage = ResultJson->GetStringField(TEXT("error"));
            }
            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = bSuccess && ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess && ResultJson->HasField(TEXT("error")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                }
            }

            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the error message
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                if (ErrorMessage.IsEmpty())
                {
                    ErrorMessage = FString::Printf(TEXT("Command failed: %s"), *CommandType);
                }
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
                // Preserve structured diagnostics on failures as well as success.
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }


    FString ResultString;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
    FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
    return ResultString;
}
