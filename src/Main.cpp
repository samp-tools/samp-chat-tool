#include <fmt/format.h>
#include <fmt/compile.h>
#include <nlohmann/json.hpp>
#include <string>
#include <fstream>
#include <iostream>
#include <map>

using json = nlohmann::json;

struct AppOptions;

auto readArgs(int argc, char* argv[])									-> std::vector< std::string_view >;
auto readFileSequentially(std::istream& inputStream_)					-> std::string;
auto readAppOptions(AppOptions& opts_, std::istream& inputStream_)		-> void;
auto parseChatJson(AppOptions const& opts_, std::istream& inputFile_)	-> std::string;

struct AppOptions
{
	// JSON field: "pch"
	// Precompiled header (optional):
	// For example: "PROJECT_PCH"
	// Will be translated to:
	// #include PROJECT_PCH
	std::string pch;

	// JSON field: "namespace"
	// Namespace of the chat messages (optional):
	// For example: "chat_txt"
	// Will be translated to:
	// namespace chat_txt
	// {
	// ...
	// }
	std::string ns;

	// JSON field: "languageEnum"
	// Language enum (optional):
	// For example: "game::Languages"
	// Will be translated to:
	// "text[game::Languages::English] = <text>"
	std::string languageEnum;

	// JSON field: "headerFiles"
	// Header files included after precompiled header (optional):
	// For example: "MyHeaderFile.h"
	// Will be translated to:
	// #include "MyHeaderFile.h"
	std::vector< std::string > headerFiles;

	// JSON field: "chatMessageType"
	// The type of each chat message (optional):
	// For example: "constexpr std::string_view"
	std::string chatMessageType = "constexpr auto";

	// JSON field: "useCompileMacro"
	// Should use FMT_COMPILE macro?
	// This should be enabled for C++20.
	bool useCompileMacro = true;

	// JSON field: "usePragmaOnce"
	// Should use #pragma once?
	// This shouldn't be disabled unless you really know what you're doing.
	bool usePragmaOnce = true;
};

constexpr std::string_view Text = "Hello, World, {}";

int main(int argc, char* argv[])
{
	auto args = readArgs(argc, argv);
	
	if (args.size() < 4)
	{
		std::cout << "Usage: " << args[0] << " [options file name] [input file name] [output file name]\n";
		return 0;
	}

	std::ifstream optsFile(args[1].data());
	if (!optsFile.is_open())
	{
		fmt::print("Error: could not open \"{}\" options file for reading.", args[1]);
		return 0;
	}

	std::ifstream inFile(args[2].data());
	if (!inFile.is_open())
	{
		fmt::print("Error: could not open \"{}\" input file for reading.", args[2]);
		return 0;
	}

	std::ofstream outFile(args[3].data());
	if (!outFile.is_open())
	{
		fmt::print("Error: could not open \"{}\" file for writing.", args[3]);
		return 0;
	}

	AppOptions opts;
	readAppOptions(opts, optsFile);

	outFile << parseChatJson(opts, inFile);
}

using LangMap = std::map<std::string, std::string>;

std::string parseChatJson(AppOptions const& opts_, std::istream& inputFile_)
{
	std::string fileContents = readFileSequentially(inputFile_);

	json j = json::parse(fileContents);

	std::string chatContent;
	chatContent.reserve(1 * 1024 * 1024);
	{
		if (j.type() != json::value_t::object)
			throw std::runtime_error("Could not parse JSON file - value is not an object.");

		LangMap langs;

		// Read languages:
		{
			auto it = j.find("languages");
			if (it == j.end() || it->type() != json::value_t::array)
				throw std::runtime_error("Could not parse JSON file - \"languages\" value is not an array.");

			for (auto const& lang : it->items())
			{
				auto const& val = lang.value();
				if (val.type() != json::value_t::object)
					throw std::runtime_error("Could not parse JSON file - language content is not an object.");

				std::string id		= val["id"].get<std::string>();
				std::string name	= val["name"].get<std::string>();
				langs[id] = name;
			}
		}

		// Read chat messages:
		{
			auto it = j.find("chatMessages");
			if (it == j.end() || it->type() != json::value_t::array)
				throw std::runtime_error("Could not parse JSON file - \"chatMessages\" field not exists or is not an array.");
			
			for(auto const& [key, value] : it->items())
			{
				if (value.type() != json::value_t::object)
					continue;

				if (!value.contains("uniqueName") || !value.contains("content"))
					continue;
				std::string uniqueName = value["uniqueName"].get<std::string>();

				std::string comment;
				std::string langContent;
				langContent.reserve(4 * 1024);
				size_t langIndex = 0;
				for (auto const& [langId, msgContent] : value["content"].items())
				{
					// Load first comment-version of a message as a comment:
					if (comment.empty())
						comment = msgContent["comment"].get<std::string>();

					langContent += "\t\tresult[";
					if (opts_.languageEnum.empty())
						langContent += std::to_string(langIndex);
					else
						langContent += "static_cast<int>(" + opts_.languageEnum + "::" + langs[langId] + ")";

					langContent += "] = ";

					if (opts_.useCompileMacro)
						langContent += "FMT_COMPILE(";

					langContent += '"';
					langContent += msgContent["processed"].get<std::string>();
					langContent += '"';

					if (opts_.useCompileMacro)
						langContent += ')';

					langContent += ";\n";
					++langIndex;
				}
					
				fmt::format_to(std::back_inserter(chatContent),
						"// \"{}\"\n"
						"class \n\t: public internal::ChatMessageBase\n"
						"{{\n"
						"\tstatic constexpr auto generateContent = []\n\t{{\n"
						"\t\tstd::array<std::string_view, {}> result;\n"
						// Each language will be appended here
						"{}"
						// End of languages
						"\t\treturn result;\n"
						"\t}};\n"
						"public:\n"
						"\tstatic constexpr auto text = generateContent();\n"
						"}} inline constexpr {};\n\n"
						"",
						comment,
						langIndex,
						langContent,
						uniqueName
					);

			}
		}
	}

	std::string output;
	output.reserve(1 * 1024 * 1024);

	// Append pragma once
	if (opts_.usePragmaOnce)
		output += "#pragma once\n\n";

	// Append pch
	if (!opts_.pch.empty())
	{
		output += "#include ";
		output += opts_.pch;
		output += '\n';
	}

	// Append header files
	for (auto const& headerFile : opts_.headerFiles)
	{
		output += "#include ";
		output += headerFile;
		output += '\n';
	}

	output += "\n\n";

	// Append namespace
	if (!opts_.ns.empty())
	{
		output += "namespace ";
		output += opts_.ns;
		output += "\n{\n\n";
	}

	output += "namespace internal {\nstruct ChatMessageBase {};\n}\n\n";
	output += chatContent;

	// Append namespace end
	if (!opts_.ns.empty())
		output += "\n}\n";

	return output;
}


////////////////////////////////////////////////
auto readArgs(int argc, char* argv[]) -> std::vector< std::string_view >
{
	auto args = std::vector< std::string_view >{};
	args.reserve(argc);
	for(int i = 0; i < argc; ++i)
		args.push_back(argv[i]);

	return args;
}

////////////////////////////////////////////////
auto readAppOptions(AppOptions& opts_, std::istream& inputStream_) -> void
{
	std::string fileContents = readFileSequentially(inputStream_);

	json j = json::parse(fileContents);

	if (j.type() != json::value_t::object)
		throw std::runtime_error("Could not parse options file - value is not an object.");

	#define READ_OPTION(CppName, CppType, JsonName, JsonType) \
		if (j.contains(JsonName)) \
		{ \
			auto const& val = j[JsonName]; \
			if (val.type() != json::value_t::JsonType) \
				throw std::runtime_error("Could not parse options file - \"" JsonName "\" value is not a "#JsonType"."); \
			opts_.CppName = j[JsonName].get<CppType>(); \
		}


	READ_OPTION(useCompileMacro,	bool, "useCompileMacro",		boolean);
	READ_OPTION(usePragmaOnce,		bool, "usePragmaOnce",			boolean);

	READ_OPTION(languageEnum,		std::string, "languageEnum",	string);
	READ_OPTION(pch,				std::string, "pch",				string);
	READ_OPTION(ns,					std::string, "namespace", 		string);
	READ_OPTION(chatMessageType,	std::string, "chatMessageType",	string);

	// Read header files:
	{
		auto headersIt = j.find("headerFiles");

		if (headersIt != j.end())
		{
			if (headersIt->type() != json::value_t::array)
				throw std::runtime_error("Could not parse options file - \"headerFiles\" field not exists or is not an array.");
			
			for(auto const& [_, value] : headersIt->items())
			{
				if (value.type() != json::value_t::string)
					continue;

				opts_.headerFiles.push_back(value.get<std::string>());
			}
		}
	}

	#undef READ_OPTION
}

////////////////////////////////////////////////
auto readFileSequentially(std::istream& inputStream_) -> std::string
{
	std::string out;
	out.reserve(1 * 1024 * 1024);


	char buffer[4 * 1024];
	while(inputStream_.read(buffer, sizeof(buffer)))
		out.append(buffer, buffer + inputStream_.gcount());

	out.append(buffer, buffer + inputStream_.gcount());

	return out;
}
