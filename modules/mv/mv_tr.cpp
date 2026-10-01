/**************************************************************************/
/*  mv_tr.cpp                                                             */
/**************************************************************************/

// Japanese editor strings for this module, added on top of the engine's translations.

#ifdef TOOLS_ENABLED

#include "mv_tr.h"

#include "core/string/translation.h"
#include "core/string/translation_server.h"

const char *MVTr::SECRET_HINT =
		"Add one Secret here to hold the server settings.\n"
		"Room size (room_max), reconnect grace (keep_sec) and the address to connect to (room_host) appear in the inspector.\n"
		"Code and values inside Secret stay on the server, so scoring and passwords are safe there.\n"
		"It is optional. Without it: 8 per room, 10 seconds grace, 127.0.0.1:4433.";

// Editor strings. Secret setting names are not put in the inspector table.
// That table is shared with the engine, and short words like Host or Port would change other types' labels too
static const char *EDITOR_JA[][2] = {
	{ MVTr::SECRET_HINT,
			"ここへ Secret を1つ置くと、サーバの設定をまとめて書けます。\n"
			"1部屋の人数(room_max)、切れた人を待つ秒数(keep_sec)、繋ぐ先(room_host)がインスペクタに出ます。\n"
			"Secretの中の本文と値はサーバに残るので、点数の計算や合言葉の確認を安心して置けます。\n"
			"無くても遊べます。そのときは1部屋8人、10秒待つ、127.0.0.1:4433 で動きます。" },
	{ "Online: Start the local test container", "Online: 手元の試験用コンテナを起動（サーバ書き出し → docker compose up → 証明書を ca へ）" },
	{ "Online: Stop the local test container", "Online: 手元の試験用コンテナを停止" },
	{ nullptr, nullptr },
};

// Build and register this module's Japanese translations into the editor translation domain
void MVTr::install() {
	Ref<Translation> tr;
	tr.instantiate();
	tr->set_locale("ja");
	for (int i = 0; EDITOR_JA[i][0] != nullptr; i++) {
		tr->add_message(String::utf8(EDITOR_JA[i][0]), String::utf8(EDITOR_JA[i][1]));
	}
	TranslationServer::get_singleton()->get_editor_domain()->add_translation(tr);
}

#endif
