#include "brgr_archive.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
TEST_CASE("DLOG 0x3f8 mission offer dialog") {
  if (!std::filesystem::exists("EV Nova/Nova.rez")) SKIP("no rez");
  const auto dlog = NovaResource_LoadDialogDefinition(0x3f8);
  REQUIRE(dlog.has_value());
  WARN("DLOG 0x3f8 rect " << dlog->top << "," << dlog->left << ","
       << dlog->bottom << "," << dlog->right
       << " ditl=" << dlog->dialog_item_list_id);
  const auto items = NovaResource_LoadDialogItems(dlog->dialog_item_list_id);
  REQUIRE(items.has_value());
  for (std::size_t i = 0; i < items->size(); ++i) {
    WARN("  item " << i << " type=" << (int)(*items)[i].type
         << " rect=" << (*items)[i].top << "," << (*items)[i].left << ","
         << (*items)[i].bottom << "," << (*items)[i].right
         << " title='" << (*items)[i].title << "'"
         << " refcon=" << (*items)[i].refcon);
  }
}
