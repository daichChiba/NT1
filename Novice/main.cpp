#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <Novice.h>
// #ifdef USE_IMGUI
#include <imgui.h>
// #endif

const char kWindowTitle[] = "LE3C_12_チバ_ダイチ";

// -----------------------------
// フェーズ定義（UI 表示用）
// -----------------------------
enum class HttpPhase { Idle, Sending, Waiting, HeadersDone, BodyReceiving, Completed, Error, Canceled };

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
	case HttpPhase::BodyReceiving:
		return "Body receiving";
	case HttpPhase::Completed:
		return "Completed";
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

static std::string PrettyJson(const std::string& s, int indentSize = 2) {
	std::string out;
	out.reserve(s.size() * 2);
	bool inString = false, escape = false;
	int indent = 0;
	auto indentSpaces = [&](int n) { out.append(n * indentSize, ' '); };
	for (char c : s) {
		if (inString) {
			out.push_back(c);
			if (escape)
				escape = false;
			else if (c == '\\')
				escape = true;
			else if (c == '"')
				inString = false;
			continue;
		}
		switch (c) {
		case ' ':
		case '\t':
		case '\r':
		case '\n': // 文字列外の空白は無視
			break;
		case '{':
		case '[':
			out.push_back(c);
			out.push_back('\n');
			indent++;
			indentSpaces(indent);
			break;
		case '}':
		case ']':
			out.push_back('\n');
			indent--;
			indentSpaces(indent);
			out.push_back(c);
			break;
		case ',':
			out.push_back(c);
			out.push_back('\n');
			indentSpaces(indent);
			break;
		case ':':
			out.push_back(':');
			out.push_back(' ');
			break;
		case '"':
			inString = true;
			out.push_back(c);
			break;
		default:
			out.push_back(c);
		}
	}
	return out;
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

	// 受信本文バッファ（UTF-8想定の生バイト）
	std::string responseBody;
	size_t totalRead = 0;

	// 現在の WinHttpReadData で使っている読み取りバッファ
	// DATA_AVAILABLE で resize() して、READ_COMPLETE で append します
	std::vector<char> inflightBuffer;
};

// ハンドル掃除
static void CloseAll(HttpAsyncState& state) {
	// inflightBuffer もクリアしておくことで、中断時に未解放メモリが残らないようにします
	state.inflightBuffer.clear();

	if (state.request) {
		// WinHTTPのハンドルはWinHttpCloseHandleで都度破棄しないとカーネルリソースがリークします
		WinHttpCloseHandle(state.request);
		state.request = nullptr;
	}
	if (state.connect) {
		WinHttpCloseHandle(state.connect);
		state.connect = nullptr;
	}
	if (state.session) {
		// セッションを閉じると、その配下の接続ハンドルもまとめて無効化されます
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
	state.responseBody.clear();
	state.totalRead = 0;
	state.inflightBuffer.clear();
}

// -----------------------------
// WinHTTP コールバック
// -----------------------------
static void CALLBACK HttpCallback(HINTERNET, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD infoLen) {
	auto* state = reinterpret_cast<HttpAsyncState*>(ctx);
	if (!state) {
		return;
	}

	switch (status) {
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: {
		// 送信完了 → レスポンス受信開始
		state->phase = HttpPhase::Waiting;
		WinHttpReceiveResponse(state->request, nullptr);
	} break;

	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
		// ヘッダー到着
		DWORD code = 0, size = sizeof(code);
		if (WinHttpQueryHeaders(state->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
			state->statusCode = code;
			state->phase = HttpPhase::HeadersDone;
		}

		// 本文読み取りを開始
		state->phase = HttpPhase::BodyReceiving;
		WinHttpQueryDataAvailable(state->request, nullptr);
	} break;

	case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
		// 読み取り可能なサイズ（バイト数）
		DWORD bytesAvailable = 0;
		if (info && infoLen == sizeof(DWORD)) {
			bytesAvailable = *reinterpret_cast<DWORD*>(info);
		}

		if (bytesAvailable == 0) {
			// 本文の終端
			state->phase = HttpPhase::Completed;

			// 読み取り完了後にクローズ（UI表示のため responseBody は保持）
			CloseAll(*state);
		} else {
			// inflightBuffer を必要サイズに確保
			state->inflightBuffer.resize(bytesAvailable);

			// 非同期読み取りを要求
			if (!WinHttpReadData(state->request, state->inflightBuffer.data(), bytesAvailable, nullptr)) {
				// 読み取り開始に失敗
				state->error = true;
				state->errorCode = GetLastError();
				state->phase = HttpPhase::Error;

				state->inflightBuffer.clear();
				CloseAll(*state);
			}
		}
	} break;

	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
		// WinHttpReadData() の第二引数に渡したアドレスが info として戻ってくる契約
		// infoLen は実際に読み取れたバイト数。0なら終端。
		if (info) {
			if (infoLen > 0) {
				// inflightBuffer の先頭から infoLen バイト分を body に追記
				state->responseBody.append(state->inflightBuffer.data(), state->inflightBuffer.data() + infoLen);
				state->totalRead += infoLen;

				// 今回ぶんは処理済みなのでクリア
				state->inflightBuffer.clear();

				// 続きを問い合わせ
				WinHttpQueryDataAvailable(state->request, nullptr);
			} else {
				// 0バイト → もう終わり
				state->phase = HttpPhase::Completed;
				state->inflightBuffer.clear();
				CloseAll(*state);
			}
		} else {
			// info==nullptr は異常ケースとして扱います
			state->error = true;
			state->phase = HttpPhase::Error;
			state->errorCode = ERROR_INVALID_DATA;

			state->inflightBuffer.clear();
			CloseAll(*state);
		}
	} break;

	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
		// 非同期API内部でエラーが発生
		state->error = true;
		if (info && infoLen == sizeof(WINHTTP_ASYNC_RESULT)) {
			auto* asyncResult = reinterpret_cast<WINHTTP_ASYNC_RESULT*>(info);
			state->errorCode = asyncResult->dwError;
		}
		state->phase = HttpPhase::Error;

		state->inflightBuffer.clear();
		CloseAll(*state);
	} break;

	default:
		break;
	}
}

// -----------------------------
// 送信開始 / キャンセル
// -----------------------------
static bool StartHttpRequest(
    HttpAsyncState& state,
    const wchar_t* method,  // L"GET" / L"POST" ...
    const wchar_t* host,    // 例: L"jsonplaceholder.typicode.com"
    const wchar_t* path,    // 例: L"/todos/1"
    const wchar_t* headers, // 例: L"Accept: application/json\r\nUser-Agent: ...\r\n"
    const char* body,       // GETならnullptr
    DWORD bodyLength        // GETなら0
) {
	if (state.active) {
		return false;
	}
	ResetFlags(state);

	// 非同期セッション
	state.session = WinHttpOpen(L"realtime-rest-check/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
	if (!state.session) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		return false;
	}

	// HTTPS 443 で接続
	state.connect = WinHttpConnect(state.session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!state.connect) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}

	// メソッド指定
	state.request = WinHttpOpenRequest(state.connect, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!state.request) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}

	// 受け取りたいイベント
	const DWORD kCallbackFlags = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
	                             WINHTTP_CALLBACK_FLAG_REQUEST_ERROR;

	WinHttpSetStatusCallback(state.request, &HttpCallback, kCallbackFlags, 0);

	// コールバックから HttpAsyncState* にアクセスできるようにする
	DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
	WinHttpSetOption(state.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));

	state.phase = HttpPhase::Sending;
	state.active = true;

	// 送信（GETなら body=nullptr/len=0）
	BOOL ok = WinHttpSendRequest(state.request, headers, static_cast<DWORD>(-1), (LPVOID)body, bodyLength, bodyLength, 0);

	if (!ok) {
		state.error = true;
		state.errorCode = GetLastError();
		state.phase = HttpPhase::Error;
		CloseAll(state);
		return false;
	}
	return true;
}

static void CancelHttp(HttpAsyncState& state) {
	state.phase = HttpPhase::Canceled;
	state.inflightBuffer.clear();
	CloseAll(state);
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

	// --- ISSの位置情報 ---
	const wchar_t* kHostName = L"api.wheretheiss.at";
	const wchar_t* kPath = L"v1/satellites/25544";

	// ヘッダ（\r\n 必須）。User-Agent は一部のAPIで必須です。
	// Acceptには application/json
	// を指定してJSONレスポンスを要求します。そのとおりに応えてくれるかはサーバー次第。
	const wchar_t* kHttpHeaders = L"Accept: application/json\r\n"
	                              L"User-Agent: NoviceWinHTTP/1.0\r\n";

	HttpAsyncState asyncState;

	while (Novice::ProcessMessage() == 0) {
		Novice::BeginFrame();

		std::memcpy(preKeys, keys, 256);
		Novice::GetHitKeyStateAll(keys);

		// #ifdef USE_IMGUI
		//  Window表示
		ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_Once);
		ImGui::Begin("HTTP Async Control & Monitor", nullptr, ImGuiWindowFlags_NoCollapse);

		// 行1: 操作ボタンとフェーズ表示
		{
			bool canConnect = !asyncState.active &&
			                  (asyncState.phase == HttpPhase::Idle || asyncState.phase == HttpPhase::Completed || asyncState.phase == HttpPhase::Error || asyncState.phase == HttpPhase::Canceled);

			if (canConnect) {
				if (ImGui::Button("Connect")) {
					StartHttpRequest(asyncState, L"GET", kHostName, kPath, kHttpHeaders, nullptr, 0);
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
			std::string path8 = ConvertString(kPath);
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
			ImGui::Text("HTTP %lu", asyncState.statusCode);
			ImGui::BulletText("Headers received.");

		} else if (asyncState.phase == HttpPhase::BodyReceiving) {
			ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "Receiving body...");
			ImGui::BulletText("Read: %zu bytes", asyncState.totalRead);
			ImGui::TextDisabled("request=%p connect=%p session=%p", asyncState.request, asyncState.connect, asyncState.session);

		} else if (asyncState.phase == HttpPhase::Completed) {
			ImVec4 col = (asyncState.statusCode >= 200 && asyncState.statusCode < 300) ? ImVec4(0.3f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.7f, 0.2f, 1.0f);

			ImGui::TextColored(col, "HTTP %lu (Completed, %zu bytes)", asyncState.statusCode, asyncState.totalRead);

			ImGui::Separator();
			// ImGuiWindowFlags childFlags = ImGuiWindowFlags_None;
			//  0.0f で「現在のウィンドウ幅に合わせて折り返し」
			ImGui::PushTextWrapPos(0.0f);
			// json整形
			std::string pretty = PrettyJson(asyncState.responseBody);
			// 内容の表示
			ImGui::InputTextMultiline("##json", pretty.data(), pretty.size(), ImVec2(0, 260), ImGuiInputTextFlags_ReadOnly);
			ImGui::PopTextWrapPos();

		} else if (asyncState.phase == HttpPhase::Sending || asyncState.phase == HttpPhase::Waiting) {

			ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "Waiting response...");
			ImGui::BulletText(asyncState.phase == HttpPhase::Sending ? "Sending request..." : "Awaiting headers...");
			ImGui::TextDisabled("request=%p connect=%p session=%p", asyncState.request, asyncState.connect, asyncState.session);

		} else if (asyncState.phase == HttpPhase::Canceled) {
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Canceled");

		} else {
			// Idle
			ImGui::TextDisabled("Idle. Press Connect to start.");
		}

		ImGui::End();
		// #endif // USE_IMGUI

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