#pragma once

namespace zn {
class Vault;
}

namespace zn::demo {

// Fills a fresh vault with notes that show every feature: styles, checklists, tables, tags, diagrams.
void populate(Vault& vault);

}  // namespace zn::demo
