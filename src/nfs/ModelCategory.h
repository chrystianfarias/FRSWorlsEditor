#pragma once

#include <string>
#include <vector>

namespace nfs {

// Which shelf a model goes on in the editor's catalogue.
//
// The artists tagged every model with a prefix, and the prefix is a category
// already: XB_ buildings, XO_ objects, XS_ signs, XW_ walls, XT_ trees, XV_
// vehicles, TRN_ terrain, ZPM_ race graphics. Across L4RA's 8,613 models only
// about a hundred fall outside those. The larger groups mix very different
// things -- a lamp post and a container crane are both XO_ -- so they are cut
// again by words in the rest of the name.
struct ModelCategory {
    std::wstring group;   // "Objects"
    std::wstring sub;     // "Lights"; empty when the group is not divided
};

ModelCategory CategorizeModel(const std::string& name);

// The groups in the order the catalogue lists them.
const std::vector<std::wstring>& ModelCategoryOrder();

} // namespace nfs
