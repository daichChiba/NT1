#include <iostream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>

#include <windows.h>
#include <cstring>

#include <Novice.h>
#ifdef USE_IMGUI
#include <imgui.h>
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "libssl.lib")
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "ixwebsocket.lib")

const char kWindowTitle[] = "LE3C_12_チバ_ダイチ";

std::string makeJoinMessage(std::string_view topic, std::string_view userToken) {
    std::string message;
    message.reserve(128);

    message += R"({"topic":")";
    message += topic;
    message += R"(","event":"phx_join","payload":{"user_token":")";
    message += userToken;
    message += R"("},"ref":"1"})";

    return message;
}

struct RealtimeMonitorState {
    enum class Status { Idle, Connecting, Connected, Error, Closed };

    std::mutex mutex;
    Status status = Status::Idle;
    bool joined = false;
    std::string lastError;

    void set(Status newStatus) {
        std::lock_guard<std::mutex> lock(mutex);
        status = newStatus;
        if (newStatus != Status::Connected) {
            joined = false; // 再接続時はJOIN状態をリセット
        }
    }

    void setError(const std::string& errorMessage) {
        std::lock_guard<std::mutex> lock(mutex);
        status = Status::Error;
        lastError = errorMessage;
        joined = false;
    }

    void setJoined(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        joined = value;
    }

    void snapshot(Status& outStatus, bool& outJoined, std::string& outLastError) {
        std::lock_guard<std::mutex> lock(mutex);
        outStatus = status;
        outJoined = joined;
        outLastError = lastError;
    }
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const int kWindowWidth = 1280;
    const int kWindowHeight = 720;
    Novice::Initialize(kWindowTitle, kWindowWidth, kWindowHeight);

    char keys[256] = { 0 };
    char previousKeys[256] = { 0 };

    const std::string supabaseUrl =
        "wss://oolchvtzizhmniggcaiw.supabase.co/realtime/v1/websocket"
        "?apikey=sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J&vsn=1.0.0";
    const std::string topic = "realtime:public:messages";
    const std::string userToken = "sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J";

    ix::initNetSystem();

    RealtimeMonitorState realtimeMonitorState;
    std::unique_ptr<ix::WebSocket> webSocket; // Connectボタン押下で生成

    auto createAndStartWebSocket = [&](RealtimeMonitorState& state) {
        if (webSocket) {
            webSocket->close();
            webSocket.reset();
        }

        webSocket = std::make_unique<ix::WebSocket>();
        webSocket->setUrl(supabaseUrl);

        ix::WebSocket* rawWebSocket = webSocket.get();
        webSocket->setOnMessageCallback([&state, rawWebSocket, &topic, &userToken](const ix::WebSocketMessagePtr& messagePtr) {
            using MessageType = ix::WebSocketMessageType;

            if (messagePtr->type == MessageType::Open) {
                state.set(RealtimeMonitorState::Status::Connected);
                const std::string joinMessage = makeJoinMessage(topic, userToken);
                rawWebSocket->sendText(joinMessage);
            }
            else if (messagePtr->type == MessageType::Message) {
                const std::string& messageText = messagePtr->str;

                const bool isPhxReply =
                    (messageText.find(R"("event":"phx_reply")") != std::string::npos);

                const bool isOkStatus =
                    (messageText.find(R"("status":"ok")") != std::string::npos) ||
                    (messageText.find(R"("response":{"status":"ok")") != std::string::npos);

                if (isPhxReply && isOkStatus) {
                    state.setJoined(true);
                }
            }
            else if (messagePtr->type == MessageType::Error) {
                state.setError(messagePtr->errorInfo.reason);
            }
            else if (messagePtr->type == MessageType::Close) {
                state.set(RealtimeMonitorState::Status::Closed);
            }
            });

        state.set(RealtimeMonitorState::Status::Connecting);
        webSocket->start();
        };

    while (Novice::ProcessMessage() == 0) {
        Novice::BeginFrame();

        std::memcpy(previousKeys, keys, 256);
        Novice::GetHitKeyStateAll(keys);

#ifdef USE_IMGUI
        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480.0f, 180.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Realtime Status", nullptr, ImGuiWindowFlags_NoCollapse);

        RealtimeMonitorState::Status statusSnapshot = RealtimeMonitorState::Status::Idle;
        bool joinedSnapshot = false;
        std::string lastErrorText;
        realtimeMonitorState.snapshot(statusSnapshot, joinedSnapshot, lastErrorText);

        const bool canStartConnection =
            statusSnapshot == RealtimeMonitorState::Status::Idle ||
            statusSnapshot == RealtimeMonitorState::Status::Error ||
            statusSnapshot == RealtimeMonitorState::Status::Closed;

        ImGui::BeginDisabled(!canStartConnection);
        if (ImGui::Button("Connect")) {
            createAndStartWebSocket(realtimeMonitorState);
        }
        ImGui::EndDisabled();

        const char* statusText = "Idle";
        ImVec4 statusColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

        if (statusSnapshot == RealtimeMonitorState::Status::Connecting) {
            statusText = "Connecting...";
            statusColor = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
        }
        else if (statusSnapshot == RealtimeMonitorState::Status::Connected) {
            statusText = "Connected";
            statusColor = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
        }
        else if (statusSnapshot == RealtimeMonitorState::Status::Error) {
            statusText = "Error";
            statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        }
        else if (statusSnapshot == RealtimeMonitorState::Status::Closed) {
            statusText = "Closed";
            statusColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        }

        ImGui::Separator();
        ImGui::Text("Status:");
        ImGui::SameLine();
        ImGui::ColorButton("##connection_indicator", statusColor, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
        ImGui::SameLine();
        ImGui::TextUnformatted(statusText);

        if (statusSnapshot == RealtimeMonitorState::Status::Connected) {
            ImVec4 joinColor;
            if (joinedSnapshot) {
                joinColor = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
            }
            else {
                joinColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
            }

            ImGui::Separator();
            ImGui::Text("Join:");
            ImGui::SameLine();
            ImGui::ColorButton("##join_indicator", joinColor, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
            ImGui::SameLine();
            ImGui::TextUnformatted(joinedSnapshot ? "OK (phx_reply: ok)" : "Waiting…");
        }

        if (statusSnapshot == RealtimeMonitorState::Status::Error && !lastErrorText.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("Error: %s", lastErrorText.c_str());
        }

        ImGui::End();
#endif

        Novice::EndFrame();

        if (previousKeys[DIK_ESCAPE] == 0 && keys[DIK_ESCAPE] != 0) {
            break;
        }
    }

    if (webSocket) {
        webSocket->close();
    }
    ix::uninitNetSystem();
    Novice::Finalize();
    return 0;
}
