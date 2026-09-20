#include "brgr_archive.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

// T4 custom-art ordinals: the reader's DLOG 0xbbc blits the dësc variant
// PICT into DITL entry 2, and the offer's DLOG 0x3fc into entry 8 (Ghidra
// NovaUi_DrawSelectionDialogContent 0x00499870 / NovaUi_DrawMissionShip-
// InteractionWindow 0x00447680). The port maps entry N to item index N-1, so
// pin the two target ordinals here.
TEST_CASE("variant dialogs carry the reader/offer art ordinals") {
  if (!std::filesystem::exists("EV Nova/Nova.rez"))
    SKIP("no rez");
  const auto dlog = NovaResource_LoadDialogDefinition(0xbbc);
  REQUIRE(dlog.has_value());
  const auto items = NovaResource_LoadDialogItems(dlog->dialog_item_list_id);
  REQUIRE(items.has_value());
  REQUIRE(items->size() == 6);
  CHECK((*items)[1].index == 1); // DITL entry 2
  CHECK((*items)[1].right - (*items)[1].left == 200);
  CHECK((*items)[1].bottom - (*items)[1].top == 200);

  const auto offer_dlog = NovaResource_LoadDialogDefinition(0x3fc);
  REQUIRE(offer_dlog.has_value());
  const auto offer_items =
      NovaResource_LoadDialogItems(offer_dlog->dialog_item_list_id);
  REQUIRE(offer_items.has_value());
  REQUIRE(offer_items->size() == 10);
  CHECK((*offer_items)[7].index == 7); // DITL entry 8
  // The variant DITL 0x3fc also carries the entry-6 can't-refuse accept.
  CHECK((*offer_items)[5].index == 5); // entry 6
  CHECK((*offer_items)[5].left == 170);
  CHECK((*offer_items)[5].top == 213);
}

TEST_CASE("DLOG 0x3f8 mission offer dialog") {
  if (!std::filesystem::exists("EV Nova/Nova.rez"))
    SKIP("no rez");
  const auto dlog = NovaResource_LoadDialogDefinition(0x3f8);
  REQUIRE(dlog.has_value());
  WARN("DLOG 0x3f8 rect " << dlog->top << "," << dlog->left << ","
                          << dlog->bottom << "," << dlog->right
                          << " ditl=" << dlog->dialog_item_list_id);
  const auto items = NovaResource_LoadDialogItems(dlog->dialog_item_list_id);
  REQUIRE(items.has_value());
  // DITL 1016 item 5 is UiPanel entry 6, the single accept button the
  // Flags-0x0004 ("can't refuse") arm draws/hit-tests (0x004a1820 /
  // 0x004a1670). Pin its rect so a layout regression is caught.
  REQUIRE(items->size() == 10);
  CHECK((*items)[0].index == 0); // entry 1 accept
  CHECK((*items)[1].index == 1); // entry 2 decline
  CHECK((*items)[5].index == 5); // entry 6 can't-refuse accept
  CHECK((*items)[5].left == 173);
  CHECK((*items)[5].top == 285);
  CHECK((*items)[5].right - (*items)[5].left == 99);
  CHECK((*items)[5].bottom - (*items)[5].top == 25);
  for (std::size_t i = 0; i < items->size(); ++i) {
    WARN("  item " << i << " type=" << (int)(*items)[i].type
                   << " rect=" << (*items)[i].top << "," << (*items)[i].left
                   << "," << (*items)[i].bottom << "," << (*items)[i].right
                   << " title='" << (*items)[i].title << "'"
                   << " refcon=" << (*items)[i].refcon);
  }
}
