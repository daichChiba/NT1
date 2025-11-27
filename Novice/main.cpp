#include <chrono>
#include <cstring>
#include <iomanip>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "nlohmann/json.hpp"

#include <Novice.h>
#ifdef USE_IMGUI
#include <imgui.h>
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "libssl.lib")
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "ixwebsocket.lib")

using json = nlohmann::json;
const char kWindowTitle[] = "LE3C_12_チバ_ダイチ";

// 設定
namespace Config {
// Supabase Realtime WebSocket の URLプロジェクト固有のエンドポイント
const std::string kSupabaseUrl = "wss://oolchvtzizhmniggcaiw.supabase.co/realtime/v1/websocket"
                                 "?apikey=sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J&vsn=1.0.0";
// Phoenix チャンネルのトピック（例: public:messages）
const std::string kTopic = "realtime:public:messages";
// 認証トークン（ここでは publishable key を使用）実運用では認証済みトークン
const std::string kUserToken = "sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J";
// ハートビート間隔（フレーム数）60fps 想定で約3秒
const int32_t kHeartbeatIntervalFrames = 180;
} // namespace Config

// ユーティリティ関数群
namespace Utils {
// 簡易 UUID 判定ハイフン位置と全長のみ検査
// 厳密検査時は正規表現の利用を推奨
bool IsUUID(const std::string& candidate) {
	if (candidate.length() != 36) {
		return false;
	}
	if (candidate[8] != '-' || candidate[13] != '-' || candidate[18] != '-' || candidate[23] != '-') {
		return false;
	}
	return true;
}

// 現在時刻を ISO8601（UTC）形式に整形する関数
std::string GetCurrentTimeISO8601() {
	auto nowTimePoint = std::chrono::system_clock::now();
	auto timeT = std::chrono::system_clock::to_time_t(nowTimePoint);
	std::tm timeInfo;
	gmtime_s(&timeInfo, &timeT); // UTC 変換
	std::ostringstream outputStream;
	outputStream << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");
	return outputStream.str();
}

// 現在時刻をローカル表示用に整形する関数
std::string GetCurrentTimeLocal() {
	auto nowTimePoint = std::chrono::system_clock::now();
	auto timeT = std::chrono::system_clock::to_time_t(nowTimePoint);
	std::tm timeInfo;
	localtime_s(&timeInfo, &timeT); // ローカル変換
	std::ostringstream outputStream;
	outputStream << std::put_time(&timeInfo, "%H:%M:%S");
	return outputStream.str();
}
} // namespace Utils

// メッセージ生成ファクトリ
namespace MessageFactory {
// Phoenix Channel の join メッセージ生成
std::string MakeJoinMessage(const std::string& topic, const std::string& userToken) {
	json jsonObject;
	jsonObject["topic"] = topic;                     // チャンネルトピック
	jsonObject["event"] = "phx_join";                // join イベント
	jsonObject["ref"] = "1";                         // 参照 ID（任意）
	jsonObject["payload"]["user_token"] = userToken; // 認証情報

	return jsonObject.dump();
}

// Heartbeat（心拍）メッセージ生成
std::string MakeHeartbeatMessage() {
	json jsonObject;
	jsonObject["topic"] = "phoenix";        // システムトピック
	jsonObject["event"] = "heartbeat";      // heartbeat
	jsonObject["payload"] = json::object(); // 空オブジェクト
	jsonObject["ref"] = "hb";               // 参照 ID

	return jsonObject.dump();
}

// Presence の track メッセージ生成自身のオンライン通知用
std::string MakePresenceTrackMessage(const std::string& topic, const std::string& userName) {
	std::string isoTime = Utils::GetCurrentTimeISO8601();

	json jsonObject;
	jsonObject["topic"] = topic;
	jsonObject["event"] = "presence"; // presence イベント
	jsonObject["ref"] = "presence_1";

	jsonObject["payload"]["event"] = "track"; // track サブイベント
	jsonObject["payload"]["payload"]["user_name"] = userName;
	jsonObject["payload"]["payload"]["online_at"] = isoTime;

	return jsonObject.dump();
}
} // namespace MessageFactory

// Presence 関連データ構造
struct PresenceUser {
	std::string uniqueId;    // UUID
	std::string userName;    // 表示名
	std::string onlineAt;    // ローカル表示用のオンライン時刻
	int32_t joinedFrame = 0; // 参加フレーム番号
};

// JSON パース用エントリ
struct PresenceEntry {
	std::string uniqueId;
	std::string userName;
};

// Presence 管理クラス（スレッド安全）
class PresenceManager {
public:
	// ユーザ追加既存なら無操作
	void AddUser(const std::string& uniqueId, const std::string& userName, int32_t frame) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (users_.find(uniqueId) == users_.end()) {
			PresenceUser user;
			user.uniqueId = uniqueId;
			user.userName = userName;
			user.joinedFrame = frame;
			user.onlineAt = Utils::GetCurrentTimeLocal();

			users_[uniqueId] = user;

			std::string logMessage = "[" + user.onlineAt + "] " + userName + " joined";
			AddActivityMessage(logMessage);
		}
	}

	// ユーザ削除存在しなければ無操作
	void RemoveUser(const std::string& uniqueId, const std::string& providedUserName) {
		std::lock_guard<std::mutex> lock(mutex_);

		auto iterator = users_.find(uniqueId);
		if (iterator != users_.end()) {
			std::string displayName;
			if (iterator->second.userName.empty()) {
				displayName = providedUserName;
			} else {
				displayName = iterator->second.userName;
			}

			std::string logMessage = "[" + Utils::GetCurrentTimeLocal() + "] " + displayName + " left";
			AddActivityMessage(logMessage);

			users_.erase(iterator);
		}
	}

	// 全削除再接続時の初期化用
	void Clear() {
		std::lock_guard<std::mutex> lock(mutex_);
		users_.clear();
		activityMessages_.clear();
	}

	// ユーザ集合のコピー取得表示用
	std::map<std::string, PresenceUser> GetUsers() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return users_;
	}

	// 活動ログのコピー取得表示用
	std::vector<std::string> GetActivityMessages() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return activityMessages_;
	}

private:
	mutable std::mutex mutex_; // const メソッドからのロック用
	std::map<std::string, PresenceUser> users_;
	std::vector<std::string> activityMessages_;
	const size_t kMaxMessages_ = 10; // ログ上限

	// ログ追加上限超過時は古いものを削除
	void AddActivityMessage(const std::string& message) {
		activityMessages_.push_back(message);
		if (activityMessages_.size() > kMaxMessages_) {
			activityMessages_.erase(activityMessages_.begin());
		}
	}
};

// Realtime 接続状態クラス
class RealtimeConnectionState {
public:
	enum class Status { Idle, Connecting, Connected, Error, Closed };

	struct Snapshot {
		Status status = Status::Idle;           // 接続状態
		bool joined = false;                    // チャンネル join 状態
		std::string lastError;                  // 最後のエラー
		int32_t lastHeartbeatSentFrame = 0;     // 最後に送信したフレーム番号
		int32_t lastHeartbeatReceivedFrame = 0; // 最後に受信したフレーム番号
	};

	// 状態更新はロックで保護
	void SetStatus(Status newStatus) {
		std::lock_guard<std::mutex> lock(mutex_);
		status_ = newStatus;
		if (newStatus != Status::Connected) {
			joined_ = false; // 切断時は joined クリア
		}
	}

	void SetError(const std::string& errorMessage) {
		std::lock_guard<std::mutex> lock(mutex_);
		status_ = Status::Error;
		lastError_ = errorMessage;
		joined_ = false;
	}

	void SetJoined(bool value) {
		std::lock_guard<std::mutex> lock(mutex_);
		joined_ = value;
	}

	void RecordHeartbeatSent(int32_t frame) {
		std::lock_guard<std::mutex> lock(mutex_);
		lastHeartbeatSentFrame_ = frame;
	}

	void RecordHeartbeatReceived(int32_t frame) {
		std::lock_guard<std::mutex> lock(mutex_);
		lastHeartbeatReceivedFrame_ = frame;
	}

	// スナップショット取得コピー返却
	Snapshot GetSnapshot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		Snapshot snapshot;
		snapshot.status = status_;
		snapshot.joined = joined_;
		snapshot.lastError = lastError_;
		snapshot.lastHeartbeatSentFrame = lastHeartbeatSentFrame_;
		snapshot.lastHeartbeatReceivedFrame = lastHeartbeatReceivedFrame_;
		return snapshot;
	}

private:
	mutable std::mutex mutex_;
	Status status_ = Status::Idle;
	bool joined_ = false;
	std::string lastError_;
	int32_t lastHeartbeatSentFrame_ = 0;
	int32_t lastHeartbeatReceivedFrame_ = 0;
};

// JSON パースヘルパ
namespace PresenceParser {
// presence_diff の解析joins/leaves を出力する変化有無を返す
bool ParsePresenceDiff(const std::string& jsonString, std::vector<PresenceEntry>& outJoins, std::vector<PresenceEntry>& outLeaves) {
	if (!nlohmann::json::accept(jsonString)) {
		return false;
	}

	json parsedJson = json::parse(jsonString, nullptr, false);
	if (parsedJson.is_discarded() || !parsedJson.contains("payload")) {
		return false;
	}

	json payloadObject = parsedJson["payload"];

	if (payloadObject.contains("joins") && payloadObject["joins"].is_object()) {
		for (auto& item : payloadObject["joins"].items()) {
			const std::string candidateKey = item.key();
			const json& valueObject = item.value();

			if (Utils::IsUUID(candidateKey)) {
				if (valueObject.contains("metas") && !valueObject["metas"].empty()) {
					json meta = valueObject["metas"][0];
					if (meta.contains("user_name")) {
						std::string userName = meta["user_name"].get<std::string>();
						outJoins.push_back({candidateKey, userName});
					}
				}
			}
		}
	}

	if (payloadObject.contains("leaves") && payloadObject["leaves"].is_object()) {
		for (auto& item : payloadObject["leaves"].items()) {
			const std::string candidateKey = item.key();
			const json& valueObject = item.value();

			if (Utils::IsUUID(candidateKey)) {
				std::string userNameFallback = candidateKey;
				if (valueObject.contains("metas") && !valueObject["metas"].empty()) {
					json meta = valueObject["metas"][0];
					if (meta.contains("user_name")) {
						userNameFallback = meta["user_name"].get<std::string>();
					}
				}
				outLeaves.push_back({candidateKey, userNameFallback});
			}
		}
	}

	return !outJoins.empty() || !outLeaves.empty();
}

// presence_state の解析全体状態の取り出し
void ParsePresenceState(const std::string& jsonString, std::vector<PresenceEntry>& outEntries) {
	if (!nlohmann::json::accept(jsonString)) {
		return;
	}

	json parsedJson = json::parse(jsonString, nullptr, false);
	if (parsedJson.is_discarded() || !parsedJson.contains("payload")) {
		return;
	}

	json payloadObject = parsedJson["payload"];
	if (payloadObject.is_object()) {
		for (auto& item : payloadObject.items()) {
			const std::string candidateKey = item.key();
			const json& valueObject = item.value();

			if (Utils::IsUUID(candidateKey)) {
				if (valueObject.contains("metas") && !valueObject["metas"].empty()) {
					json meta = valueObject["metas"][0];
					if (meta.contains("user_name")) {
						std::string userName = meta["user_name"].get<std::string>();
						outEntries.push_back({candidateKey, userName});
					}
				}
			}
		}
	}
}
} // namespace PresenceParser

// UI 描画補助
namespace UI {
// 接続状態描画色はステータスに応じて変化
void DrawConnectionStatus(const RealtimeConnectionState::Snapshot& snapshot) {
	const char* statusText = "Idle";
	ImVec4 statusColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

	if (snapshot.status == RealtimeConnectionState::Status::Connecting) {
		statusText = "Connecting...";
		statusColor = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
	} else if (snapshot.status == RealtimeConnectionState::Status::Connected) {
		statusText = "Connected";
		statusColor = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
	} else if (snapshot.status == RealtimeConnectionState::Status::Error) {
		statusText = "Error";
		statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
	} else if (snapshot.status == RealtimeConnectionState::Status::Closed) {
		statusText = "Closed";
		statusColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
	}

	ImGui::Separator();
	ImGui::Text("Status:");
	ImGui::SameLine();
	ImGui::ColorButton("##conn", statusColor, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
	ImGui::SameLine();
	ImGui::TextUnformatted(statusText);
}

// Join 状態描画
void DrawJoinStatus(bool joined) {
	ImVec4 joinColor;
	if (joined) {
		joinColor = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
	} else {
		joinColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
	}

	ImGui::Separator();
	ImGui::Text("Join:");
	ImGui::SameLine();
	ImGui::ColorButton("##join", joinColor, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
	ImGui::SameLine();
	ImGui::TextUnformatted(joined ? "OK" : "Waiting...");
}

// ハートビートステータス描画
void DrawHeartbeatStatus(int32_t currentFrame, const RealtimeConnectionState::Snapshot& snapshot) {
	const int32_t warningFrames = 300; // 警告閾値（フレーム数）
	const int32_t sinceLastReceive = currentFrame - snapshot.lastHeartbeatReceivedFrame;

	ImVec4 colorVector;
	if (snapshot.lastHeartbeatReceivedFrame == 0) {
		colorVector = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
	} else if (sinceLastReceive >= warningFrames) {
		colorVector = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
	} else {
		colorVector = ImVec4(0.2f, 1.0f, 0.3f, 1.0f);
	}

	ImGui::Separator();
	ImGui::Text("Heartbeat:");
	ImGui::SameLine();
	ImGui::ColorButton("##hb", colorVector, ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));

	ImGui::Indent();
	ImGui::Text("Last Sent Frame: %d", snapshot.lastHeartbeatSentFrame);
	ImGui::Text("Last Recv Frame: %d", snapshot.lastHeartbeatReceivedFrame);
	ImGui::Text("Current Frame: %d", currentFrame);
	ImGui::Unindent();
}

// Presence 情報描画ユーザ一覧とログ
void DrawPresenceInfo(const PresenceManager& presenceManager) {
	auto users = presenceManager.GetUsers();
	auto messages = presenceManager.GetActivityMessages();

	ImGui::Separator();
	ImGui::Text("Users (%zu):", users.size());

	ImGui::BeginChild("Users", ImVec2(0, 120), true);
	for (const auto& pair : users) {
		const std::string id = pair.first;
		const PresenceUser& user = pair.second;
		std::string shortId;
		if (id.length() > 8) {
			shortId = id.substr(0, 8) + "...";
		} else {
			shortId = id;
		}
		ImGui::BulletText("%s [%s]", user.userName.c_str(), shortId.c_str());
	}
	ImGui::EndChild();

	ImGui::Separator();
	ImGui::Text("Log:");
	ImGui::BeginChild("Log", ImVec2(0, 150), true);
	for (const auto& messageStr : messages) {
		ImGui::TextWrapped("%s", messageStr.c_str());
	}
	ImGui::EndChild();
}
} // namespace UI

// メイン関数：WinMain
int32_t WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t) {
	// 簡易ウィンドウ初期化（Novice ライブラリ）
	Novice::Initialize(kWindowTitle, 1280, 720);

	// ImGui を使用する場合の日本語フォント読み込み処理
#ifdef USE_IMGUI
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear(); // 既存フォントクリア

	ImFont* loadedFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/meiryo.ttc", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());

	if (loadedFont == NULL) {
		OutputDebugStringA("Failed to load Japanese font, using default");
		io.Fonts->AddFontDefault();
		io.Fonts->GetGlyphRangesJapanese(); // 日本語範囲要求
	} else {
		OutputDebugStringA("Successfully loaded Japanese font");
	}

	io.Fonts->Build(); // フォントテクスチャビルド
#endif

	char keys[256] = {0};
	char previousKeys[256] = {0};

	std::string displayUserName = "LE3C_12_チバ_ダイチ"; // 表示用ユーザ名

	// ixwebsocket 初期化
	ix::initNetSystem();
	RealtimeConnectionState connectionState;
	PresenceManager presenceManager;

	std::unique_ptr<ix::WebSocket> webSocketPtr;
	uint32_t currentFrame = 0;
	bool presenceTrackSent = false; // presence トラック一回送信フラグ

	// StartWebSocket ラムダWebSocket を生成して接続開始
	auto StartWebSocket = [&]() {
		// 既存接続が存在する場合はクローズ
		if (webSocketPtr) {
			webSocketPtr->close();
			webSocketPtr.reset();
		}
		presenceManager.Clear();
		presenceTrackSent = false;

		webSocketPtr = std::make_unique<ix::WebSocket>();
		webSocketPtr->setUrl(Config::kSupabaseUrl);

		// メッセージ受信コールバック登録
		webSocketPtr->setOnMessageCallback([&](const ix::WebSocketMessagePtr& messagePtr) {
			if (messagePtr->type == ix::WebSocketMessageType::Open) {
				OutputDebugStringA("WS OPEN");
				connectionState.SetStatus(RealtimeConnectionState::Status::Connected);
				std::string joinMessage = MessageFactory::MakeJoinMessage(Config::kTopic, Config::kUserToken);
				webSocketPtr->sendText(joinMessage);

			} else if (messagePtr->type == ix::WebSocketMessageType::Message) {
				std::string receivedText = messagePtr->str;

				bool isPhoenixReply = (receivedText.find("phx_reply") != std::string::npos);
				bool isStatusOk = (receivedText.find("\"status\":\"ok\"") != std::string::npos);

				if (isPhoenixReply && isStatusOk) {
					if (receivedText.find("\"ref\":\"1\"") != std::string::npos) {
						connectionState.SetJoined(true);
						if (!presenceTrackSent) {
							std::string presenceMessage = MessageFactory::MakePresenceTrackMessage(Config::kTopic, displayUserName);
							webSocketPtr->sendText(presenceMessage);
							presenceTrackSent = true;
						}
					} else if (receivedText.find("\"ref\":\"hb\"") != std::string::npos) {
						connectionState.RecordHeartbeatReceived(static_cast<int32_t>(currentFrame));
					}
				}

				if (receivedText.find("presence_state") != std::string::npos) {
					std::vector<PresenceEntry> parsedEntries;
					PresenceParser::ParsePresenceState(receivedText, parsedEntries);
					for (const auto& entry : parsedEntries) {
						presenceManager.AddUser(entry.uniqueId, entry.userName, static_cast<int32_t>(currentFrame));
					}
				}

				if (receivedText.find("presence_diff") != std::string::npos) {
					std::vector<PresenceEntry> joinEntries;
					std::vector<PresenceEntry> leaveEntries;
					bool hasChanges = PresenceParser::ParsePresenceDiff(receivedText, joinEntries, leaveEntries);
					if (hasChanges) {
						for (const auto& joinEntry : joinEntries) {
							presenceManager.AddUser(joinEntry.uniqueId, joinEntry.userName, static_cast<int32_t>(currentFrame));
						}
						for (const auto& leaveEntry : leaveEntries) {
							presenceManager.RemoveUser(leaveEntry.uniqueId, leaveEntry.userName);
						}
					}
				}

			} else if (messagePtr->type == ix::WebSocketMessageType::Error) {
				connectionState.SetError(messagePtr->errorInfo.reason);

			} else if (messagePtr->type == ix::WebSocketMessageType::Close) {
				connectionState.SetStatus(RealtimeConnectionState::Status::Closed);
			}
		});

		connectionState.SetStatus(RealtimeConnectionState::Status::Connecting);
		webSocketPtr->start(); // 非同期接続開始
	};

	// メインループ
	while (Novice::ProcessMessage() == 0) {
		Novice::BeginFrame();
		memcpy(previousKeys, keys, 256);
		Novice::GetHitKeyStateAll(keys);
		++currentFrame;

		auto snapshot = connectionState.GetSnapshot();

		// ハートビート送信判定join 済みかつ所定フレーム経過で送信
		if (webSocketPtr && snapshot.joined) {
			int32_t frameDifference = static_cast<int32_t>(currentFrame) - snapshot.lastHeartbeatSentFrame;
			if (frameDifference >= Config::kHeartbeatIntervalFrames) {
				webSocketPtr->sendText(MessageFactory::MakeHeartbeatMessage());
				connectionState.RecordHeartbeatSent(static_cast<int32_t>(currentFrame));
			}
		}

#ifdef USE_IMGUI
		ImGui::Begin("Status");
		if (ImGui::Button("Connect")) {
			StartWebSocket();
		}
		ImGui::SameLine();
		ImGui::Text("Name: %s", displayUserName.c_str());
		UI::DrawConnectionStatus(snapshot);
		if (snapshot.status == RealtimeConnectionState::Status::Connected) {
			UI::DrawJoinStatus(snapshot.joined);
			UI::DrawHeartbeatStatus(static_cast<int32_t>(currentFrame), snapshot);
			UI::DrawPresenceInfo(presenceManager);
		}
		ImGui::End();
#endif

		Novice::EndFrame();
		if (keys[DIK_ESCAPE] && !previousKeys[DIK_ESCAPE]) {
			break; // ESC で終了
		}
	}

	// 終了処理
	if (webSocketPtr) {
		webSocketPtr->close();
	}
	ix::uninitNetSystem();
	Novice::Finalize();
	return 0;
}