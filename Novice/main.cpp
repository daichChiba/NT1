#include <cstring>
#include <iostream>
#include <string>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <Novice.h>

const char kWindowTitle[] = "LE3C_12_チバ_ダイチ";

// Windowsアプリでのエントリーポイント(main関数)
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

	// ヘッダーは 改行コードが CRLF 必須のため \r\n を明示して記述する
	const wchar_t* kHttpHeaders = L"Content-Type: application/json\r\n"
	                              L"apikey: sb_publishable_kcL7fFe5hC-ruqdcW0Yjdg_lsRXWu3J\r\n";

	// JSON 本文は生文字列に変更（C++の生文字列リテラルを利用）
	// https://cpprefjp.github.io/lang/cpp11/raw_string_literals.html
	const char* kBody = R"JSON({
  "messages": [
    {
      "topic": "test",
      "event": "msg",
      "payload": { "text": "hi" }
    }
  ]
})JSON";

	// WinHTTP はサービス向けの HTTPクライアント。
	// まずセッションを開き、以降の操作でこのハンドルを共有する。
	HINTERNET session = WinHttpOpen(L"realtime-rest-check/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
	if (!session) {
		OutputDebugStringA("WinHttpOpen failed");
		return 1;
	}

	// セッションから接続ハンドルを生成し、接続先のホスト名とポートを確定する。
	HINTERNET connect = WinHttpConnect(session, kHostName, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!connect) {
		OutputDebugStringA("WinHttpConnect failed");
		return 1;
	}

	// 接続ハンドルから HTTP リクエストを構築し、メソッドや HTTPS フラグをここで設定する。
	HINTERNET hRequest = WinHttpOpenRequest(connect, L"POST", kConnectionPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		OutputDebugStringA("WinHttpOpenRequest failed");
		return 1;
	}

	DWORD bodyLength = static_cast<DWORD>(std::strlen(kBody));

	// 送信（ヘッダー + 本文）
	// ヘッダーと JSON ボディをまとめて送信。送信バッファの管理はアプリ側で行う。
	BOOL ok = WinHttpSendRequest(hRequest, kHttpHeaders, static_cast<DWORD>(-1), static_cast<LPVOID>(const_cast<char*>(kBody)), bodyLength, bodyLength, 0);
	if (!ok) {
		OutputDebugStringA("WinHttpSendRequest failed");
		return 1;
	}

	// サーバー応答の到着を待機。TLS ハンドシェイクもこの呼び出し中に完了する。
	ok = WinHttpReceiveResponse(hRequest, nullptr);
	if (!ok) {
		OutputDebugStringA("WinHttpReceiveResponse failed");
		return 1;
	}

	// ステータスコード取得
	// 受信済みリクエストから HTTP ステータスコードなどのヘッダーを取得する。
	DWORD status = 0;
	DWORD size = sizeof(status);
	ok = WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

	if (!ok) {
		OutputDebugStringA("WinHttpQueryHeaders failed");
		return 1;
	}

	// ウィンドウの×ボタンが押されるまでループ
	while (Novice::ProcessMessage() == 0) {
		// フレームの開始
		Novice::BeginFrame();

		// キー入力を受け取る
		std::memcpy(preKeys, keys, 256);
		Novice::GetHitKeyStateAll(keys);

		///
		/// ↓更新処理ここから
		///

		///
		/// ↑更新処理ここまで
		///

		///
		/// ↓描画処理ここから
		///
		Novice::ScreenPrintf(0, 0, "%lu", status);

		///
		/// ↑描画処理ここまで
		///

		// フレームの終了
		Novice::EndFrame();

		// ESCキーが押されたらループを抜ける
		if (preKeys[DIK_ESCAPE] == 0 && keys[DIK_ESCAPE] != 0) {
			break;
		}
	}

	// 後片付け
	// WinHTTP のハンドルは作成した順とは逆順で必ず解放する
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);

	// ライブラリの終了
	Novice::Finalize();
	return 0;
}