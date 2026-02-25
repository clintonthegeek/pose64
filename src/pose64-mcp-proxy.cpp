/* -*- mode: C++; tab-width: 4 -*- */
/**
 * pose64-mcp-proxy.cpp — MCP-to-ReControl TCP bridge
 *
 * Bridges JSON-RPC (MCP protocol over stdio) to the POSE64 emulator's
 * ReControl text-command TCP server.
 *
 *   Claude Code <--stdin/stdout JSON-RPC--> pose64-mcp-proxy <--TCP--> emulator:6416
 *
 * Single-threaded.  No Qt, no httplib, no threads.  POSIX sockets + nlohmann/json.
 * Target: C++17.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "json.hpp"

using json = nlohmann::json;

// ============================================================================
// Globals
// ============================================================================

static std::string g_host = "127.0.0.1";
static int         g_port = 6416;
static int         g_sock = -1;

// ============================================================================
// Logging (all to stderr — stdout is clean JSON-RPC only)
// ============================================================================

static void log_msg (const std::string& msg)
{
    fprintf (stderr, "[pose64-mcp-proxy] %s\n", msg.c_str ());
}

// ============================================================================
// TCP helpers
// ============================================================================

static int tcp_connect (const std::string& host, int port)
{
    int fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        log_msg ("socket() failed: " + std::string (strerror (errno)));
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons (port);

    if (inet_pton (AF_INET, host.c_str (), &addr.sin_addr) <= 0)
    {
        log_msg ("inet_pton() failed for host: " + host);
        close (fd);
        return -1;
    }

    if (connect (fd, (struct sockaddr*) &addr, sizeof (addr)) < 0)
    {
        log_msg ("connect() failed: " + std::string (strerror (errno)));
        close (fd);
        return -1;
    }

    return fd;
}

static bool tcp_send (int fd, const std::string& msg)
{
    size_t total = 0;
    while (total < msg.size ())
    {
        ssize_t n = send (fd, msg.data () + total, msg.size () - total, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        total += (size_t) n;
    }
    return true;
}

// Read until '\n'.  Returns true on success, false on error/EOF.
// The line is returned WITHOUT the trailing newline.
static bool tcp_recv_line (int fd, std::string& out)
{
    out.clear ();
    char ch;
    while (true)
    {
        ssize_t n = recv (fd, &ch, 1, 0);
        if (n <= 0)
            return false;  // error or EOF
        if (ch == '\n')
            return true;
        out += ch;
    }
}

// Read multiple lines until a line containing only ".".
// Returns true on success.  Result includes all lines (including the first
// OK line) joined by newline, but excludes the terminating "." line.
static bool tcp_recv_multiline (int fd, std::string& result)
{
    result.clear ();
    std::string line;
    while (true)
    {
        if (!tcp_recv_line (fd, line))
            return false;
        if (line == ".")
            return true;
        if (!result.empty ())
            result += '\n';
        result += line;
    }
}

// ============================================================================
// Reconnect logic
// ============================================================================

static bool tcp_reconnect ()
{
    if (g_sock >= 0)
    {
        close (g_sock);
        g_sock = -1;
    }
    log_msg ("reconnecting to " + g_host + ":" + std::to_string (g_port));
    g_sock = tcp_connect (g_host, g_port);
    return g_sock >= 0;
}

// Ensure we have a TCP connection.  Returns true if connected.
static bool tcp_ensure_connected ()
{
    if (g_sock >= 0)
        return true;
    log_msg ("connecting to " + g_host + ":" + std::to_string (g_port));
    g_sock = tcp_connect (g_host, g_port);
    if (g_sock >= 0)
        log_msg ("connected");
    return g_sock >= 0;
}

// Send a command and receive a single-line response, with one reconnect attempt.
static std::string rc_command (const std::string& cmd)
{
    std::string full = cmd + "\n";
    std::string resp;

    if (!tcp_ensure_connected () || !tcp_send (g_sock, full) || !tcp_recv_line (g_sock, resp))
    {
        if (!tcp_reconnect () || !tcp_send (g_sock, full) || !tcp_recv_line (g_sock, resp))
            return "ERR transient: TCP connection lost";
    }
    return resp;
}

// Send a command and receive a multi-line response, with one reconnect attempt.
// Multi-line commands (ui, info, apps) return "OK ...\n" first line, then more lines
// terminated by ".\n".  But if the command fails, ReControl returns "ERR ...\n"
// as a single line (no dot terminator).  So we read the first line, and only
// continue to multi-line read if it starts with "OK".
static std::string rc_command_multi (const std::string& cmd)
{
    std::string full = cmd + "\n";
    std::string first_line;

    if (!tcp_ensure_connected () || !tcp_send (g_sock, full) || !tcp_recv_line (g_sock, first_line))
    {
        if (!tcp_reconnect () || !tcp_send (g_sock, full) || !tcp_recv_line (g_sock, first_line))
            return "ERR transient: TCP connection lost";
    }

    // If first line is ERR, return it immediately (no dot terminator follows)
    if (first_line.substr (0, 3) == "ERR")
        return first_line;

    // Read remaining lines until "."
    std::string rest;
    if (!tcp_recv_multiline (g_sock, rest))
        return "ERR transient: TCP connection lost during multi-line read";

    if (rest.empty ())
        return first_line;
    return first_line + "\n" + rest;
}

// ============================================================================
// JSON-RPC response helpers
// ============================================================================

// Convert a Latin-1 (ISO-8859-1) string to valid UTF-8 so nlohmann/json
// won't throw type_error.316 when serialising text from the emulator.
// ASCII bytes (0x00-0x7F) pass through unchanged; bytes 0x80-0xFF are
// encoded as two-byte UTF-8 sequences (U+0080 through U+00FF).
static std::string latin1_to_utf8 (const std::string& s)
{
    std::string out;
    out.reserve (s.size ());
    for (unsigned char c : s)
    {
        if (c < 0x80)
        {
            out += (char) c;
        }
        else
        {
            // Two-byte UTF-8: 110xxxxx 10xxxxxx
            out += (char) (0xC0 | (c >> 6));
            out += (char) (0x80 | (c & 0x3F));
        }
    }
    return out;
}

static json make_result (const json& id, const json& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json make_error (const json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

static json make_tool_result (const json& id, const std::string& text, bool is_error = false)
{
    json content = json::array ();
    content.push_back ({{"type", "text"}, {"text", latin1_to_utf8 (text)}});
    json result = {{"content", content}};
    if (is_error)
        result["isError"] = true;
    return make_result (id, result);
}

static json make_tool_image (const json& id, const std::string& base64data)
{
    json content = json::array ();
    content.push_back ({{"type", "image"}, {"data", base64data}, {"mimeType", "image/png"}});
    return make_result (id, {{"content", content}});
}

// ============================================================================
// Base64 encoder (for screenshot PNG)
// ============================================================================

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode (const std::vector<uint8_t>& data)
{
    std::string out;
    size_t len = data.size ();
    out.reserve (((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t a = data[i];
        uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? data[i + 2] : 0;

        uint32_t triple = (a << 16) | (b << 8) | c;

        out += b64_table[(triple >> 18) & 0x3F];
        out += b64_table[(triple >> 12) & 0x3F];
        out += (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64_table[(triple     ) & 0x3F] : '=';
    }

    return out;
}

// ============================================================================
// Tool definitions for tools/list
// ============================================================================

static json make_schema (json properties = {}, std::vector<std::string> required = {})
{
    json schema = {{"type", "object"}};
    if (!properties.empty ())
        schema["properties"] = properties;
    if (!required.empty ())
        schema["required"] = json (required);
    return schema;
}

static json int_prop (const std::string& desc)
{
    return {{"type", "integer"}, {"description", desc}};
}

static json str_prop (const std::string& desc)
{
    return {{"type", "string"}, {"description", desc}};
}

static json get_tools_list ()
{
    json tools = json::array ();

    auto add = [&](const std::string& name, const std::string& desc, json schema) {
        tools.push_back ({{"name", name}, {"description", desc}, {"inputSchema", schema}});
    };

    add ("palm_ping", "Ping the emulator to check if it is responsive.",
         make_schema ());

    add ("palm_state", "Get emulator state and device info.",
         make_schema ());

    add ("palm_ui", "Read the current Palm OS form/UI structure.",
         make_schema ());

    add ("palm_apps", "List installed applications on the emulated device.",
         make_schema ());

    add ("palm_dbs", "List all databases (apps, data, resources) on the emulated device.",
         make_schema ());

    add ("palm_tap", "Tap at screen coordinates.",
         make_schema ({{"x", int_prop ("X coordinate")}, {"y", int_prop ("Y coordinate")}},
                      {"x", "y"}));

    add ("palm_tap_id", "Tap a UI object by its numeric ID.",
         make_schema ({{"id", int_prop ("Object ID from palm_ui")}},
                      {"id"}));

    add ("palm_pen", "Send a pen down or up event at coordinates.",
         make_schema ({{"action", str_prop ("'down' or 'up'")},
                       {"x", int_prop ("X coordinate")},
                       {"y", int_prop ("Y coordinate")}},
                      {"action", "x", "y"}));

    add ("palm_key", "Send a key event by character code.",
         make_schema ({{"code", int_prop ("Character code")}},
                      {"code"}));

    add ("palm_type", "Type a string of text into the emulator.",
         make_schema ({{"text", str_prop ("Text to type")}},
                      {"text"}));

    add ("palm_button", "Press a hardware button (power, up, down, app1-4, cradle, contrast).",
         make_schema ({{"name", str_prop ("Button name")},
                       {"action", str_prop ("'down', 'up', or 'tap'")}},
                      {"name", "action"}));

    add ("palm_screenshot", "Take a screenshot. Returns image data if no path given, otherwise saves to path.",
         make_schema ({{"path", str_prop ("File path to save PNG (optional, default /tmp/pose64_screenshot.png)")}}));

    add ("palm_screen_hash", "Get a CRC32 hash of the current screen contents.",
         make_schema ());

    add ("palm_launch", "Launch an application by database name.",
         make_schema ({{"app", str_prop ("Application database name")}},
                      {"app"}));

    add ("palm_install", "Install a .prc/.pdb file into the emulator.",
         make_schema ({{"path", str_prop ("Path to .prc or .pdb file")}},
                      {"path"}));

    add ("palm_export", "Export a Palm OS database (.prc/.pdb) from the emulator to a file.",
         make_schema ({{"db", str_prop ("Database name (from palm_apps)")},
                       {"path", str_prop ("File path to write the exported database")}},
                      {"db", "path"}));

    add ("palm_save", "Save the current emulator session to a file.",
         make_schema ({{"path", str_prop ("Path to save session file")}},
                      {"path"}));

    add ("palm_load", "Load an emulator session from a file.",
         make_schema ({{"path", str_prop ("Path to session file")}},
                      {"path"}));

    add ("palm_reset", "Reset the emulated device.",
         make_schema ({{"type", str_prop ("'soft', 'hard', or 'debug' (optional, default soft)")}}));

    add ("palm_sleep", "Pause for a number of milliseconds (1-30000).",
         make_schema ({{"ms", int_prop ("Milliseconds to sleep (1-30000)")}},
                      {"ms"}));

    add ("palm_quit", "Quit the emulator.",
         make_schema ());

    add ("palm_dialog", "Query or respond to a pending POSE64 modal dialog (debugger, warnings, etc).",
         make_schema ({{"respond", str_prop ("Button to click: ok, cancel, continue, debug, reset, yes, no (omit to just query)")}}));

    add ("palm_run", "Execute a batch of commands in one call. Commands separated by semicolons. "
         "Supports: tap, pen, key, type, button, sleep, repeat N { ... }. "
         "Example: 'tap 12 148; sleep 150; type x; tap 12 148'",
         make_schema ({{"script", str_prop ("Semicolon-separated commands to execute")}},
                      {"script"}));

    add ("palm_peek", "Read bytes from emulated memory. Address formats: 0x<hex> (absolute), "
         "a5@<offset> (A5-relative), global.<name> (low-memory global).",
         make_schema ({{"addr", str_prop ("Memory address (0x<hex>, a5@<offset>, or global.<name>)")},
                       {"nbytes", int_prop ("Number of bytes to read (1-256)")}},
                      {"addr", "nbytes"}));

    add ("palm_poke", "Write bytes to emulated memory.",
         make_schema ({{"addr", str_prop ("Memory address (0x<hex>, a5@<offset>, or global.<name>)")},
                       {"nbytes", int_prop ("Number of bytes to write (1-256)")},
                       {"data", str_prop ("Hex string of bytes to write (e.g. '00A1B2C3')")}},
                      {"addr", "nbytes", "data"}));

    add ("palm_regs", "Read m68k CPU registers (D0-D7, A0-A7, PC, SR).",
         make_schema ());

    add ("palm_menu", "Trigger a menu item by menu and item title. Posts a menuEvent to the Palm OS event queue.",
         make_schema ({{"menu", str_prop ("Menu title (e.g. 'Options')")},
                       {"item", str_prop ("Item title or substring (e.g. 'About')")}},
                      {"menu", "item"}));

    add ("palm_delete", "Delete a database from the emulated device.",
         make_schema ({{"db", str_prop ("Database name to delete")}},
                      {"db"}));

    return tools;
}

// ============================================================================
// Tool dispatch
// ============================================================================

// Helper: send single-line command, return tool result (OK text or ERR).
static json rc_tool (const json& id, const std::string& cmd)
{
    std::string resp = rc_command (cmd);
    bool ok = resp.substr (0, 2) == "OK";
    return make_tool_result (id, resp, !ok);
}

// Helper: send multi-line command, return tool result.
static json rc_tool_multi (const json& id, const std::string& cmd)
{
    std::string resp = rc_command_multi (cmd);
    bool is_err = (resp.size () >= 3 && resp.substr (0, 3) == "ERR");
    return make_tool_result (id, resp, is_err);
}

static json dispatch_tool (const json& id, const std::string& name, const json& args)
{
    if (name == "palm_ping")
    {
        std::string resp = rc_command ("state");
        if (resp.substr (0, 2) == "OK")
            return make_tool_result (id, "pong");
        return make_tool_result (id, resp, true);
    }

    if (name == "palm_state")
    {
        std::string state = rc_command ("state");
        std::string info  = rc_command_multi ("info");
        return make_tool_result (id, json ({{"state", state}, {"info", info}}).dump (2));
    }

    if (name == "palm_ui")           return rc_tool_multi (id, "ui");
    if (name == "palm_apps")         return rc_tool_multi (id, "apps");
    if (name == "palm_dbs")          return rc_tool_multi (id, "apps all");

    if (name == "palm_tap")
        return rc_tool (id, "tap " + std::to_string (args.value ("x", 0))
                                  + " " + std::to_string (args.value ("y", 0)));

    if (name == "palm_tap_id")
        return rc_tool (id, "tap-id " + std::to_string (args.value ("id", 0)));

    if (name == "palm_pen")
        return rc_tool (id, "pen " + args.value ("action", std::string ())
                                  + " " + std::to_string (args.value ("x", 0))
                                  + " " + std::to_string (args.value ("y", 0)));

    if (name == "palm_key")
        return rc_tool (id, "key " + std::to_string (args.value ("code", 0)));

    if (name == "palm_type")
        return rc_tool (id, "type " + args.value ("text", std::string ()));

    if (name == "palm_button")
        return rc_tool (id, "button " + args.value ("name", std::string ())
                                     + " " + args.value ("action", std::string ()));

    if (name == "palm_screenshot")
    {
        std::string path = args.value ("path", std::string ());
        bool has_path = !path.empty ();
        if (!has_path)
            path = "/tmp/pose64_screenshot.png";

        std::string resp = rc_command ("screenshot " + path);
        if (resp.substr (0, 2) != "OK")
            return make_tool_result (id, resp, true);

        if (has_path)
            return make_tool_result (id, resp);

        // No path given — read the PNG and return as base64 image
        std::ifstream file (path, std::ios::binary);
        if (!file)
            return make_tool_result (id, "ERR: could not read " + path, true);

        std::vector<uint8_t> data ((std::istreambuf_iterator<char> (file)),
                                    std::istreambuf_iterator<char> ());
        return make_tool_image (id, base64_encode (data));
    }

    if (name == "palm_screen_hash")  return rc_tool (id, "screen-hash");
    if (name == "palm_launch")       return rc_tool (id, "launch " + args.value ("app", std::string ()));
    if (name == "palm_install")      return rc_tool (id, "install " + args.value ("path", std::string ()));
    if (name == "palm_export")       return rc_tool (id, "export " + args.value ("db", std::string ())
                                                              + " " + args.value ("path", std::string ()));
    if (name == "palm_save")         return rc_tool (id, "save " + args.value ("path", std::string ()));
    if (name == "palm_load")         return rc_tool (id, "load " + args.value ("path", std::string ()));

    if (name == "palm_reset")
    {
        std::string type = args.value ("type", std::string ());
        return rc_tool (id, type.empty () ? "reset" : "reset " + type);
    }

    if (name == "palm_sleep")
        return rc_tool (id, "sleep " + std::to_string (args.value ("ms", 0)));

    if (name == "palm_quit")         return rc_tool (id, "quit");

    if (name == "palm_dialog")
    {
        std::string respond = args.value ("respond", std::string ());
        if (respond.empty ())
        {
            std::string info = rc_command_multi ("dialog");
            return make_tool_result (id, info);
        }
        else
        {
            return rc_tool (id, "dialog respond " + respond);
        }
    }

    if (name == "palm_run")
        return rc_tool (id, "run " + args.value ("script", std::string ()));

    if (name == "palm_peek")
        return rc_tool (id, "peek " + args.value ("addr", std::string ())
                                    + " " + std::to_string (args.value ("nbytes", 0)));

    if (name == "palm_poke")
        return rc_tool (id, "poke " + args.value ("addr", std::string ())
                                    + " " + std::to_string (args.value ("nbytes", 0))
                                    + " " + args.value ("data", std::string ()));

    if (name == "palm_regs")
        return rc_tool (id, "regs");

    if (name == "palm_menu")
        return rc_tool (id, "menu \"" + args.value ("menu", std::string ())
                                      + "\" \"" + args.value ("item", std::string ()) + "\"");

    if (name == "palm_delete")
        return rc_tool (id, "delete " + args.value ("db", std::string ()));

    return make_tool_result (id, "Unknown tool: " + name, true);
}

// ============================================================================
// MCP method handlers
// ============================================================================

static json handle_initialize (const json& id, const json& /* params */)
{
    return make_result (id, {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {{"tools", json::object ()}}},
        {"serverInfo", {{"name", "pose64-mcp-proxy"}, {"version", "1.0.0"}}}
    });
}

static json handle_tools_list (const json& id)
{
    return make_result (id, {{"tools", get_tools_list ()}});
}

static json handle_tools_call (const json& id, const json& params)
{
    std::string name = params.value ("name", "");
    json arguments = params.value ("arguments", json::object ());
    return dispatch_tool (id, name, arguments);
}

// ============================================================================
// Main
// ============================================================================

int main (int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            g_port = std::atoi (argv[++i]);
        else if (arg == "--host" && i + 1 < argc)
            g_host = argv[++i];
        else if (arg == "--help" || arg == "-h")
        {
            fprintf (stderr,
                "Usage: pose64-mcp-proxy [--host HOST] [--port PORT]\n"
                "  --host HOST  ReControl server host (default: 127.0.0.1)\n"
                "  --port PORT  ReControl server port (default: 6416)\n");
            return 0;
        }
    }

    log_msg ("ready — TCP connection deferred until first tool call");

    std::string line;
    while (std::getline (std::cin, line))
    {
        if (line.empty ())
            continue;

        // Strip trailing CR if present
        if (!line.empty () && line.back () == '\r')
            line.pop_back ();

        json msg;
        try
        {
            msg = json::parse (line);
        }
        catch (const json::exception& e)
        {
            log_msg ("invalid JSON on stdin: " + std::string (e.what ()));
            continue;
        }

        std::string method = msg.value ("method", "");
        json id = msg.contains ("id") ? msg["id"] : json (nullptr);
        json params = msg.value ("params", json::object ());

        // Notifications (no id) — handle silently
        if (id.is_null ())
        {
            // "notifications/initialized" and others — no response needed
            continue;
        }

        json response;

        if (method == "initialize")
            response = handle_initialize (id, params);
        else if (method == "tools/list")
            response = handle_tools_list (id);
        else if (method == "tools/call")
            response = handle_tools_call (id, params);
        else
            response = make_error (id, -32601, "Method not found");

        std::cout << response.dump () << "\n" << std::flush;
    }

    log_msg ("stdin closed, exiting");
    if (g_sock >= 0)
        close (g_sock);

    return 0;
}
