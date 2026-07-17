/*
 * english_dict.js — HEB ENG MIX FIX (v3)
 * ----------------------------------------------------------------------------
 * Common English words, used as the precision gate for the reverse direction:
 * a token typed as Hebrew glyphs is only treated as wrong-layout ENGLISH when
 * its reverse-mapped form is a real English word (or the token violates Hebrew
 * final-letter orthography). Mirrors the role hebrew_dict.js plays for EN→HE.
 * ----------------------------------------------------------------------------
 */
(function (factory) {
  const data = factory();
  if (typeof window !== "undefined") window.EN_WORDS = data.set;
  if (typeof module !== "undefined" && module.exports) module.exports = data;
})(function () {
  "use strict";

  const EN_WORDS_LIST = ("a i am an as at be by do go he if in is it me my no of on or so to up us we " +
    "and are but can did for get got had has her him his how its let man new not now off old one out own say " +
    "see she the too two use was way who why yes yet you all any bad big boy day end far few fun guy job key " +
    "kid lot mad map mom dad net oil pay per put ran red run sad sat set sit six sun ten the top try war win " +
    "able about above after again alone along also always among angry animal another answer around asked away " +
    "back based basic beautiful because become been before began begin behind being believe below best better " +
    "between beyond black body book both bring brought build business call came cannot care carry case catch " +
    "cause certain change check child children city class clean clear close cold come common company complete " +
    "computer could country course create cut dark data days dear deep does doesnt doing done dont door down " +
    "during each early earth easy eat eight either else end english enough even ever every everyone everything exactly " +
    "example experience eyes face fact family fast father feel feet felt find fine fire first five follow food " +
    "form found four free friend friends from front full game gave give given going gone good great green group " +
    "grow guess half hand happy hard have head hear heard heart hebrew hello help here high hold home hope hour house " +
    "however huge human hundred idea important improve inside instead into issue just keep kind knew know known " +
    "language large last late later learn least leave left less level life light like line list little live " +
    "local long look love made make many matter maybe mean means might mind mine minute miss moment money month " +
    "more morning most mother move much music must myself name near need needs never next nice night nine none " +
    "note nothing number often once only open order other others our over page paper part past people perhaps " +
    "person phone place plan play please point power pretty problem program project pull push question quick " +
    "quite rather read ready real really reason remember report rest right room round said same saw school " +
    "second seem seen send sense sent seven shall short should show side simple since small smart some someone " +
    "something sometimes soon sorry sound speak special stand start state still stop story street strong stuff " +
    "such sure system take taken talk team tell text than thank thanks that their them then there these they " +
    "thing things think third this those though thought three through time today together told took total touch " +
    "toward true trust turn under until upon used user very view voice wait walk want water week well went were " +
    "what when where which while white whole will wish with within without word words work world would write " +
    "wrong year years young your yourself").split(/\s+/);

  return { list: EN_WORDS_LIST, set: new Set(EN_WORDS_LIST) };
});
