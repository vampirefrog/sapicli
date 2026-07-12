<?xml version="1.0" encoding="UTF-8"?>
<!--
  sapicli custom pronunciations. W3C PLS 1.0 format:
    https://www.w3.org/TR/pronunciation-lexicon/

  * xml:lang on <lexicon> is the default language for lexemes.
    Individual <lexeme> elements can override it with their own xml:lang.
  * alphabet must be "x-microsoft-sapi" (sapicli feeds the phoneme
    string straight to ISpPhoneConverter::PhoneToId, which is SAPI's
    phoneme table; see MSDN "American English Phoneme Table" and the
    per-language equivalents).
  * Multiple <grapheme>s per <lexeme> = aliases: each grapheme gets the
    same pronunciation.
  * If a <lexeme> has multiple <phoneme>s (PLS pronunciation variants)
    sapicli takes the first, because SAPI's AddPronunciation only accepts
    one per (word, part-of-speech).

  Rename this file to `lexemes.pls` next to sapicli.exe to have it loaded
  automatically, or pass it explicitly with `sapicli -L <path> ...`.
  Entries are added to the SAPI user lexicon which is registry-backed and
  survives across runs / reboots.
-->
<lexicon version="1.0"
         xmlns="http://www.w3.org/2005/01/pronunciation-lexicon"
         alphabet="x-microsoft-sapi"
         xml:lang="en-US">

  <!-- Legacy hardcoded jokes (were baked into sapicli.exe before this file
       existed). Remove any you don't want. -->
  <lexeme><grapheme>cum</grapheme>   <phoneme>k uw m</phoneme></lexeme>
  <lexeme><grapheme>poo</grapheme>   <phoneme>p uw</phoneme></lexeme>
  <lexeme><grapheme>lol</grapheme>   <phoneme>l uh l</phoneme></lexeme>
  <lexeme><grapheme>lolol</grapheme> <phoneme>l uh l uh l</phoneme></lexeme>
  <lexeme><grapheme>deez</grapheme>  <phoneme>d iy z</phoneme></lexeme>
  <lexeme><grapheme>nutz</grapheme>  <phoneme>n ah t s</phoneme></lexeme>
  <lexeme><grapheme>nasim</grapheme> <phoneme>n ah s iy m</phoneme></lexeme>

  <!-- Demo entries so you can hear that the file was loaded end-to-end.
       "zapfluk" (spelled to look like "zap fluck") is given "pizza" as
       its pronunciation, so the A/B test is unmistakable. Use a fresh
       word (never AddPronunciation'd on this machine before) if you want
       a repeatable demo (the user lexicon is persistent, so once you
       add "zapfluk" once, later runs without the file still get it). -->
  <lexeme><grapheme>sapicli</grapheme>  <phoneme>s ae p iy k l iy</phoneme></lexeme>
  <lexeme><grapheme>muxaudio</grapheme> <phoneme>m ah k s ao d iy ow</phoneme></lexeme>
  <lexeme><grapheme>zapfluk</grapheme>  <phoneme>p iy t z ax</phoneme></lexeme>
  <lexeme><grapheme>wobbafunk</grapheme> <phoneme>k ax n eh r iy</phoneme></lexeme>

</lexicon>
