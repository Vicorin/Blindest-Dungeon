# Translation files for the Darkest Dungeon accessibility mod
#
# Almost everything the mod speaks comes from the game's own localization tables and follows
# the game's language setting automatically. The files in this folder cover the REST: the
# mod's own feedback lines ("No exit that way.", "Turn skipped.") that the game has no words
# for. English is built into the mod; a file here overrides it for one language.
#
# HOW IT WORKS
# - Name a file after the game's INTERNAL language name plus .txt: german.txt, french.txt,
#   spanish.txt, italian.txt, czech.txt, polish.txt, russian.txt, ... (the same names the
#   game itself uses; pick a language in the game's Options, switch on "Debug logging" in the
#   F10 mod menu, and the mod logs the name in ddaccess-debug.log in the game folder as:
#   axlang: session language "..." ).
# - These files are COMPILED INTO the mod: Mod\build.ps1 embeds every <language>.txt in this
#   folder into ddaccess.dll on every build. Nothing ships or deploys beside the game any more
#   (the old external ddaccess-lang\ folder was retired 2026-08-12). To test a new or changed
#   file, re-run Mod\build.ps1 -- or send the file to the mod's maintainer to build in.
# - The mod reads the game's language once at startup and keeps it for the session. A
#   language change in the game's Options therefore applies at the NEXT game start -- the
#   same moment the game's own text switches (it also only loads its language at startup),
#   so the mod and the game always speak the same language.
#
# FILE FORMAT
# - Plain UTF-8 text. One entry per line:  IDENTIFIER=translated text
# - Lines starting with # or ; are comments. Empty lines are ignored.
# - START FROM THE TEMPLATE: Mod\lang\english.txt lists every identifier with its
#   English text, already in this file's format -- copy it here as <language>.txt and replace
#   the text after each =. It is generated from the master list Mod\src\core\axstrings.inc
#   (each AXS(IDENTIFIER, "English") line) by Mod\gen-lang-template.ps1; re-run that after
#   editing the .inc. It lives OUTSIDE this folder on purpose: build.ps1 embeds every .txt
#   here as a language, and English is built in, so an english.txt here would be dead weight.
#   Any identifier you leave out stays English -- a partial translation is fine.
# - Keep printf placeholders (%d, %s) exactly as in the English text, in the same order.
#   An entry whose placeholders do not match is skipped (logged in ddaccess-debug.log), so a
#   typo cannot break the mod -- but do check the log after testing a new file.
# - Translate the WHOLE sentence naturally; word order may differ from English freely.
