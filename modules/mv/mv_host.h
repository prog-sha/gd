/**************************************************************************/
/*  mv_host.h                                                             */
/**************************************************************************/

// Side that runs the world. The Secret holds identity; this handles state and @online func commands.

#pragma once

#include "mv_frame.h"
#include "mv_saves.h"
#include "mv_secret_client.h"
#include "mv_runtime.h"

#include "core/crypto/crypto.h"
#include "core/object/object_id.h"
#include "scene/main/node.h"

class MVLink;

class MVHost : public Node, public MVSaves, public MVSecretLine {
	GDCLASS(MVHost, Node);

	enum {
		PORT_TRIES = 8, // Times to shift the state port until a free one is found
	};

	Ref<MVSecretClient> line; // TLS to the Secret holding identities
	Ref<MVFrame> parser; // Buffer turning received bytes back into frames
	ObjectID runtime; // Online-side world runtime
	ObjectID state; // Entry of the state link carrying the latest state
	Ref<X509Certificate> cert; // Certificate presented on the state link. Made and handed over by the Secret
	Ref<CryptoKey> key; // Key for the state link. Also from the Secret
	String cert_name; // Name written in the certificate
	String room_id;
	String grant; // Ticket from matchmaking to hold a room. Attached to the first hello
	int port = 0; // Listen port for the state link
	bool saving = false; // Whether the Secret can accept saves
	bool running = false; // Whether the world has started running
	bool told_gone = false; // Marker that room closing was reported once

	MVRuntime *runtime_node() const; // Return the live Online-side runtime
	MVLink *state_link() const; // Return the live state link entry
	void ready_line(); // TLS to the Secret is up, so announce the room
	void took(const PackedByteArray &p_data); // Dispatch frames received from the Secret
	void handle(const Dictionary &p_frame); // Handle one frame
	void begin_world(const Dictionary &p_body); // Receive the entry list and start the world
	void refuse_in(const String &p_who, const String &p_reason, bool p_again); // Tell the Secret someone could not enter
	void allow_in(const Dictionary &p_body); // Admit a person the Secret approved into the world
	void part_out(const Dictionary &p_body); // Remove a person whose identity link dropped from the world
	void state_joined(const String &p_who); // Send the full state to a person whose state link connected
	void state_asked(const String &p_who, int64_t p_id, int64_t p_uid, const String &p_fn, const Array &p_args); // Pass a command to the world and return the answer
	void lost(const String &p_reason); // The route to the Secret dropped, so close the room
	bool send(int p_type, const Dictionary &p_body); // Send one frame to the Secret
	void keep(const String &p_op, const Dictionary &p_body); // Send a save request to the Secret

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	String start(const String &p_host, int p_port, const String &p_secret_name, const Ref<X509Certificate> &p_ca, const String &p_grant); // Connect to the Secret and prepare the room
	String room() const { return room_id; } // Return this room's name

	bool is_open() const override { return saving; }
	// Ownerless things are not kept. Each room has its own world; one shared box would make them overwrite each other
	bool keeps_world() const override { return false; }
	void poll() override {}
	void load_world(const Callable &p_done) override { p_done.call(Dictionary()); }
	void save_user(const String &p_id, const String &p_kind, const Dictionary &p_values) override;

	bool call_secret(int64_t p_id, const String &p_path, const String &p_name, const Array &p_args) override;
};
