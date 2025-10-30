#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <Novice.h>
#ifdef USE_IMGUI
#include <imgui.h>
#endif

const char kWindowTitle[] = "LE3C_12_チバ_ダイチ";

// -----------------------------
// フェーズ定義（UI 表示用）
// -----------------------------
enum class HttpPhase { Idle, Sending, Waiting, HeadersDone, Error, Canceled };

static const char* PhaseName(HttpPhase p) {
	switch (p) {
	case HttpPhase::Idle:
		return "Idle";
	case HttpPhase::Sending:
		return "Sending";
	case HttpPhase::Waiting:
		return "Waiting headers";
	case HttpPhase::HeadersDone:
		return "Headers done";
	case HttpPhase::Error:
		return "Error";
	case HttpPhase::Canceled:
		return "Canceled";
	default:
		return "?";
	}
}

// -----------------------------
// 文字列
// -----------------------------
static std::string ConvertString(const wchar_t* wstr) {
	if (!wstr) {
		return {};
	}
	int length = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
	std::string str(length > 0 ? length - 1 : 0, '\0');
	if (length > 0) {
		WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str.data(), length, nullptr, nullptr);
	}
	return str;
}

// -----------------------------
// HTTP 非同期状態
// -----------------------------
struct HttpAsyncState {
	HINTERNET session = nullptr;
	HINTERNET connect = nullptr;
	HINTERNET request = nullptr;

	DWORD statusCode = 0; // ステータスコード
	bool error = false;
	DWORD errorCode = 0;

	bool active = false; // リクエスト進行中か
	HttpPhase phase = HttpPhase::Idle;
};

// ハンドル掃除
static void CloseAll(HttpAsyncState& state) {
	if (state.request) {
		// WinHTTPのハンドルはWinHttpCloseHandleで都度破棄しないとカーネルリソースがリークする
		WinHttpCloseHandle(state.request);
		state.request = nullptr;
	}
	if (state.connect) {
		// 接続ハンドルも同様に必ず閉じる
		WinHttpCloseHandle(state.connect);
		state.connect = nullptr;
	}
	if (state.session) {
		// セッションを閉じると、その配下の接続ハンドルもまとめて無効化される
		WinHttpCloseHandle(state.session);
		state.session = nullptr;
	}
	state.active = false;
}

static void ResetFlags(HttpAsyncState& state) {
	state.statusCode = 0;
	state.error = false;
	state.errorCode = 0;
	state.phase = HttpPhase::Idle;
}

// -----------------------------
// WinHTTP コールバック
// -----------------------------
static void CALLBACK HttpCallback(HINTERNET, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD) {
	auto* state = reinterpret_cast<HttpAsyncState*>(ctx);
	if (!state) {
		return;
	}

	switch (status) {
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: {
		// WinHttpSendRequest が送信を完了した瞬間に通知される
		// 非同期モードなのでここで明示的に WinHttpReceiveResponse を呼び出しレスポンス受信を開始する
		state->phase = HttpPhase::Waiting;
		WinHttpReceiveResponse(state->request, nullptr);
	} break;

	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
		// レスポンスヘッダが到着したタイミングで呼ばれる
		DWORD code = 0, size = sizeof(code);
		if (WinHttpQueryHeaders(state->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
			// 数値形式でステータスコードを取得して記録する
			state->statusCode = code;
			state->phase = HttpPhase::HeadersDone;
		}
	} break;

	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
		// 非同期API内部でエラーが発生した際の通知。dwErrorにWinHTTP/Win32のエラーコードが入る
		state->error = true;
		if (info) {
			auto* asyncResult = reinterpret_cast<WINHTTP_ASYNC_RESULT*>(info);
			state->errorCode = asyncResult->dwError;
		}
		state->phase = HttpPhase::Error;
	} break;

	default:
		break;
	}
}

// -----------------------------
// 送信開始 / キャンセル
// -----------------------------
static bool StartHttpRequest(HttpAsyncState& state, const wchar_t* host, const wchar_t* path, const wchar_t* headers, const char* body) {
	if (state.active) {
		return false;
	}
	ResetFlags(state);

	// WinHttpOpenでHTTPセッション(=ユーザーエージェント)を作成する。非同期なのでWINHTTP_FLAG_ASYNCを付与
	state.session = WinHttpOpen(L"realtime-rest-check/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	if (!state.session) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		return false;
	}

	// セッションから対象ホストへの接続ハンドルを生成。HTTPSなので443番ポートを指定
	state.connect = WinHttpConnect(state.session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!state.connect) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}

	// 実際のHTTPリクエストハンドルを作成。WINHTTP_FLAG_SECUREでTLSを有効化する
	state.request = WinHttpOpenRequest(state.connect, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!state.request) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}

	const DWORD kCallbackFlags = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR;
	// 受け取りたいイベントをビット列で指定し、コールバック関数を登録する
	WinHttpSetStatusCallback(state.request, &HttpCallback, kCallbackFlags, 0);

	DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
	// コールバックから状態へ戻るためのユーザーデータとしてHttpAsyncStateのポインタを紐づける
	WinHttpSetOption(state.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));

	DWORD bodyLength = static_cast<DWORD>(std::strlen(body));

	state.phase = HttpPhase::Sending;
	state.active = true;

	BOOL ok = WinHttpSendRequest(state.request, headers, static_cast<DWORD>(-1), (LPVOID)body, bodyLength, bodyLength, 0);
	// WinHttpSendRequestは非同期フラグが立っているとすぐに戻る。完了後はコールバックで通知される
	if (!ok) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}
	return true;
}

static void CancelHttp(HttpAsyncState& s) {
	s.phase = HttpPhase::Canceled;
	CloseAll(s);
}

// -----------------------------
// アプリ本体
// -----------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {

	// ライブラリの初期化
	const int kWindowWidth = 1280;
	const int kWindowHeight = 720;
	Novice::Initialize(kWindowTitle, kWindowWidth, kWindowHeight);

	// キー入力結果を受け取る箱
	char keys[256] = {0};
	char preKeys[256] = {0};

	// 接続先情報
	const wchar_t* kHostName = L"oolchvtzizhmniggcaiw.supabase.co";
	const wchar_t* kConnectionPath = L"/realtime/v1/api/broadcast";

	// ヘッダ（\r\n 必須）
	const wchar_t* kHttpHeaders = L"Content-Type: application/json\r\n"
	                              L"apikey: sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J\r\n";

	// JSON 本文（生文字列）
	const char* kBody = R"JSON({
	  "messages": [
		{
		  "topic": "test",
		  "event": "msg",
		  "payload": { "text": "hi" }
		}
	  ]
	})JSON";

	// HTTP 非同期状態
	HttpAsyncState asyncState;

	// ウィンドウの×ボタンが押されるまでループ
	while (Novice::ProcessMessage() == 0) {
		Novice::BeginFrame();

		std::memcpy(preKeys, keys, 256);
		Novice::GetHitKeyStateAll(keys);

		// Window表示
		ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_Once);
		ImGui::Begin("HTTP Async Control & Monitor", nullptr, ImGuiWindowFlags_NoCollapse);

		// 行1: 操作ボタンとフェーズ表示
		{
			bool canConnect = !asyncState.active &&
			                  (asyncState.phase == HttpPhase::Idle || asyncState.phase == HttpPhase::HeadersDone || asyncState.phase == HttpPhase::Error || asyncState.phase == HttpPhase::Canceled);

			if (canConnect) {
				if (ImGui::Button("Connect")) {
					StartHttpRequest(asyncState, kHostName, kConnectionPath, kHttpHeaders, kBody);
				}
				ImGui::SameLine();
				if (ImGui::Button("Reset")) {
					CancelHttp(asyncState);
					ResetFlags(asyncState);
				}
			} else {
				if (ImGui::Button("Cancel")) {
					CancelHttp(asyncState);
				}
			}

			ImGui::SameLine();
			ImGui::TextDisabled("Phase: %s", PhaseName(asyncState.phase));
		}

		ImGui::Separator();

		// 行2: 接続先情報
		{
			std::string host8 = ConvertString(kHostName);
			std::string path8 = ConvertString(kConnectionPath);
			ImGui::Text("Host: %s", host8.c_str());
			ImGui::SameLine();
			ImGui::Text("|");
			ImGui::SameLine();
			ImGui::Text("Path: %s", path8.c_str());
		}

		ImGui::Separator();

		// 行3: ステータス表示
		if (asyncState.phase == HttpPhase::Error) {
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Status: ERROR");
			ImGui::BulletText("dwError = %lu", asyncState.errorCode);
			ImGui::TextDisabled("request=%p  connect=%p  session=%p", asyncState.request, asyncState.connect, asyncState.session);
		} else if (asyncState.phase == HttpPhase::HeadersDone) {
			ImVec4 col = (asyncState.statusCode >= 200 && asyncState.statusCode < 300) ? ImVec4(0.3f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
			ImGui::TextColored(col, "HTTP %lu", asyncState.statusCode);
			ImGui::BulletText("Headers received.");
		} else if (asyncState.phase == HttpPhase::Sending || asyncState.phase == HttpPhase::Waiting) {
			ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "Waiting response...");
			ImGui::BulletText(asyncState.phase == HttpPhase::Sending ? "Sending request..." : "Awaiting headers...");
			ImGui::TextDisabled("request=%p connect=%p session=%p", asyncState.request, asyncState.connect, asyncState.session);
		} else if (asyncState.phase == HttpPhase::Canceled) {
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Canceled");
		} else { // Idle
			ImGui::TextDisabled("Idle. Press Connect to start.");
		}

		ImGui::End();

		// ヘッダー受信後は自動でハンドル解放（次回Connectをすぐ可能にする）
		if (asyncState.phase == HttpPhase::HeadersDone && asyncState.active) {
			CloseAll(asyncState);
		}

		Novice::EndFrame();

		// ESCキーで終了
		if (preKeys[DIK_ESCAPE] == 0 && keys[DIK_ESCAPE] != 0) {
			break;
		}
	}

	// 後片付け
	CancelHttp(asyncState); // 全クローズ
	Novice::Finalize();
	return 0;
}