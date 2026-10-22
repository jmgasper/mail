#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mail {
using Id = std::int64_t;
using Timestamp = std::int64_t; // UTC seconds since the Unix epoch.
enum class Provider { Local, Imap, Pop3, Gmail, ProtonBridge };
enum class Security { TLS, StartTLS };
enum class SentCopyMode { Automatic, Upload, Server, Local };

struct Endpoint {
    std::string host;
    int port = 0;
    Security security = Security::TLS;
    std::string username;
    std::string certificate; // Trusted CA/server certificate file, or PEM text; empty uses system trust.
};

struct Account {
    Id id = 0;
    std::string name;
    std::string email;
    Provider provider = Provider::Local;
    Endpoint incoming;
    Endpoint outgoing;
    bool unified = true;
    SentCopyMode sentMode = SentCopyMode::Automatic;
    std::string sentFolder; // Empty uses the server’s advertised Sent folder.
    std::string credentialId; // Random, persistent key-store identifier; not a secret.
    // Passwords and OAuth tokens belong in BKeyStore, never in this model/database.
};

struct Attachment {
    std::string name;
    std::string mediaType = "application/octet-stream";
    std::string contentId;
    std::string data; // Binary bytes. No sender-controlled filesystem paths.
    bool inlineImage = false;
};

struct Message {
    Id id = 0;
    Id account = 0;
    Id thread = 0;
    std::string remoteKey;
    std::string providerThread;
    std::string messageId;
    std::vector<std::string> references;
    std::string sender;
    std::string replyTo; // Decoded from raw MIME when building a reply.
    std::string to;
    std::string cc;
    std::string subject;
    std::string text;
    std::string html;
    std::string raw;
    std::string folder = "inbox";
    Timestamp received = 0;
    Timestamp snoozedUntil = 0;
    bool unread = true;
    bool starred = false;
    std::vector<std::string> tags;
    std::vector<Attachment> attachments;
};

struct Query {
    Id account = 0; // Zero searches all accounts included in the unified inbox.
    std::string folder = "inbox"; // Empty searches all non-trash/non-junk folders.
    std::string text;
    std::string tag;
    bool unread = false;
    bool starred = false;
    bool attachments = false;
    bool snoozedOnly = false;
    bool includeSnoozed = false;
};

struct Conversation {
    Id id = 0;
    Id account = 0;
    std::string subject;
    std::string sender;
    std::string preview;
    Timestamp latest = 0;
    int count = 0;
    int unread = 0;
    bool starred = false;
    bool attachments = false;
};

struct SmartFolder { Id id = 0; std::string name; Query query; };
struct Filter {
    Id id = 0;
    Id account = 0;
    std::string name;
    std::string senderContains;
    std::string subjectContains;
    std::string addTag;
    std::string moveTo;
    bool markRead = false;
    bool star = false;
    bool stop = false;
    bool enabled = true;
};
}
