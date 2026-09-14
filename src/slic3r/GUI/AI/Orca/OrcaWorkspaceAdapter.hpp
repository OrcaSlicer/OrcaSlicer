#pragma once

#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"

#include <functional>

namespace Slic3r::GUI {

class Plater;
struct TextureImportOptions;

// Target colors and physical feed slots are separate decisions. Shared by the
// native handoff and its regression checks; does not mutate a project.
TextureImportOptions model_import_color_options(const AI::ModelImportRequest& request);

// Anti-corruption layer between AI application contracts and Orca workspace
// implementation details. No Orca type crosses either public port.
class OrcaWorkspaceAdapter final : public AI::IPrintablePaletteProvider, public AI::IModelArtifactConsumer
{
public:
    using ImportSucceededFn = std::function<void()>;

    OrcaWorkspaceAdapter(Plater* plater, ImportSucceededFn on_import_succeeded);

    AI::PrintablePaletteSnapshot printable_palette() const override;
    AI::ModelImportResult import_artifact(const AI::ModelImportRequest& request) override;

private:
    Plater*           m_plater { nullptr };
    ImportSucceededFn m_on_import_succeeded;
};

} // namespace Slic3r::GUI
