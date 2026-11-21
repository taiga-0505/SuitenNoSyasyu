#include "EditorExtension.h"

#include <utility>

namespace EditorExtension {

std::vector<Panel> &Panels() {
  static std::vector<Panel> panels;
  return panels;
}

void AddPanel(std::string name, std::function<void(Scene *scene)> draw,
              bool openByDefault) {
  Panel panel;
  panel.name = std::move(name);
  panel.draw = std::move(draw);
  panel.open = openByDefault;
  Panels().push_back(std::move(panel));
}

} // namespace EditorExtension
