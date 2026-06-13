#include "plugins/discord_key.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string PromptLine(const char* prompt, bool required = true) {
  std::string value;
  while (true) {
    std::cout << prompt;
    std::getline(std::cin, value);
    if (!value.empty() || !required) return value;
    std::cout << "  (required, try again)\n";
  }
}

std::string YamlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else out += c;
  }
  return out;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* output_path = (argc > 1) ? argv[1] : "discord.enc";

  std::cout << "=== Bourgeon Discord relay — credential encryptor ===\n\n"
            << "Credentials are AES-256 encrypted and written to: "
            << output_path << "\n"
            << "Copy the output file to plugins/config/discord.enc in the\n"
            << "game directory. The same file works on every player PC.\n\n";

  const std::string bot_token = PromptLine("Bot token: ");
  const std::string webhook_url =
      PromptLine("Webhook URL (empty = disable outbound relay): ", false);
  const std::string guild_name = PromptLine("Guild (server) name: ");

  std::cout << "Channels to watch (one per line, empty line to finish):\n";
  std::vector<std::string> channels;
  while (true) {
    std::cout << "  channel: ";
    std::string ch;
    std::getline(std::cin, ch);
    if (ch.empty()) break;
    channels.push_back(ch);
  }
  if (channels.empty()) {
    std::cerr << "At least one channel is required.\n";
    return 1;
  }

  const std::string char_name =
      PromptLine("Fallback character name (optional): ", false);

  // Build the YAML that the DLL will parse after decryption.
  std::ostringstream yaml;
  yaml << "bot_token: \"" << YamlEscape(bot_token) << "\"\n";
  yaml << "webhook_url: \"" << YamlEscape(webhook_url) << "\"\n";
  yaml << "guild_name: \"" << YamlEscape(guild_name) << "\"\n";
  yaml << "channels_to_watch:\n";
  for (const auto& ch : channels) {
    yaml << "  - \"" << YamlEscape(ch) << "\"\n";
  }
  yaml << "char_name: \"" << YamlEscape(char_name) << "\"\n";

  std::vector<uint8_t> encrypted;
  if (!discord_crypto::Encrypt(yaml.str(), encrypted)) {
    std::cerr << "Encryption failed.\n";
    return 1;
  }

  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    std::cerr << "Cannot write to " << output_path << ".\n";
    return 1;
  }
  out.write(reinterpret_cast<char*>(encrypted.data()), encrypted.size());
  if (!out) {
    std::cerr << "Write failed.\n";
    return 1;
  }

  std::cout << "\nDone. " << encrypted.size() << " bytes written to "
            << output_path << ".\n";
  return 0;
}
