#pragma once

namespace io { class WorldFile; }

#include "nfs/CityZones.h"
#include "nfs/World.h"
#include "render/ModelThumbnails.h"
#include "ui/BlenderLink.h"
#include "ui/EditorExtension.h"
#include "ui/ModelCatalog.h"
#include "ui/OnlineLibrary.h"
#include "ui/EditsPanel.h"
#include "ui/EmbeddedGame.h"
#include "ui/OverlayBrowser.h"
#include "ui/PropertiesPanel.h"
#include "ui/TextureBrowser.h"
#include "ui/Viewport.h"

#include <windows.h>
#include <commctrl.h>

#include <map>
#include <memory>
#include <unordered_map>
#include <string>

// The editor frame: menu, region picker, section list, viewport and status bar,
// all built from native Windows controls.
class MainWindow {
public:
    bool Create(HINSTANCE instance, int showCommand);
    void Destroy();

    HWND Handle() const { return window_; }
    HACCEL Accelerators() const { return accelerators_; }
    // A panel's text field has the keyboard, so shortcuts leave the keys to it.
    bool WantsTextInput() const { return viewport_.UiWantsText(); }


    // The modeless texture browser, for the same reason.
    HWND TextureDialog() const { return textures_.Handle(); }

    // Advances the viewport. Called from the idle part of the message loop.
    void Tick(float deltaSeconds);

private:
    // How an extension (ui/EditorExtension.h) reaches the window's parts.
    friend struct ExtensionAccess;
    // The procedural roads (ui/RoadsExtension.cpp), built into every editor.
    friend struct RoadsAccess;

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnSize();
    void CreateToolbar();
    void SetLaunchLabel(const wchar_t* text);   // the toolbar's game button

    // The panels around the viewport are resized by dragging the gaps
    // between them; the sizes are kept between runs.
    enum class Splitter { None, Left, Right, Bottom };
    Splitter HitSplitter(POINT client) const;
    bool OnSplitterMouse(UINT message, LPARAM lParam);
    void LoadPanelSizes();
    void StorePanelSizes() const;
    void OnCommand(int id, int notification, HWND control);

    void RunSetup();

    // Worlds: whole sets of edits, one open at a time (io/WorldProfiles.h).
    void NewWorld();
    void OpenWorld();
    void SaveWorld();
    void SaveWorldAs();
    // The world file (io/WorldFile.h): where it is, and the fingerprint of
    // <game>\models when it was last written or opened, to see unsaved work.
    std::wstring worldPath_;
    std::string  worldSignature_;
    std::wstring WorldDisplayName() const;
    bool WriteWorldFile(const std::wstring& path);
    // Puts the world in <game>\models aside and writes `file`'s in its place.
    bool InstallWorldFile(const class io::WorldFile& file, const std::wstring& path);
    void RememberWorldFile();
    // True when the world file is up to date, or the person said what to do.
    bool ConfirmWorldFileSaved();
    // Puts the open world away for `target`, then reopens the region.
    void SwitchToWorld(const std::wstring& target, bool createNew);
    void UpdateTitle();
    bool CanSwitchWorld();              // a game folder, and the game not running
    void ShowAbout();
    void SetGameFolder(const std::wstring& folder);
    // By value, not by reference: the first thing it does is close the region,
    // and callers naturally pass the name out of the world it is about to
    // destroy.
    void OpenRegion(std::wstring region);

    // Asks where to put it, then writes the selection as OBJ + MTL + a JSON
    // manifest for Blender.
    void ExportSelectedObjects();
    void ExportSelectedCollision();
    void GenerateMinimapBuildings();   // MINIMAP PREVIEW (temporary)
    // `quick`: a save in a Blender session -- laid on the packs as they were
    // when the session began, reported in the status bar, not in a dialog.
    void ImportEditedCollision(const std::wstring& path, bool quick = false);
    // Edit in Blender with collision polygons selected: they go over, with
    // the drawn world around them as a reference to model against.
    void EditCollisionInBlender();
    bool blenderCollision_ = false;
    std::map<std::string, nfs::CollisionPack> blenderBases_;

    // Scenery placement: a model from the region, by name, at the point the
    // view is centred on. Placements live in the editor until written.
    // The edits in force in the region, to revert one by one.
    void ShowRegionEditList();
    void RevertTickedEdits();
    // Fills the properties panel from the selection, and takes its edits back.
    void UpdateProperties();
    void ApplyPropertyFields(bool rehome);
    // `onlyMine` opens the list on the objects made in this editor.
    void PlaceObject(bool onlyMine = false);
    // Where a new placement goes: the ground under the view centre, and the
    // section that takes it. Says why not when there is none.
    bool PlacementPoint(Vec3& point, size_t& target);
    // Places a model at the view centre, asking before copying one in from
    // another section -- from the Library tab or the scene tree.
    void PlaceModelAtView(u32 hash);
    void PlaceModel(u32 hash, bool copy, size_t target, const Vec3& point);

    // Dragging a model from the Library tab onto the map. The placement is
    // made as the pointer enters the view and follows the ground under it;
    // letting go outside the view, or Escape, takes it back.
    void BeginLibraryDrag(u32 hash);
    void UpdateLibraryDrag();
    void EndLibraryDrag(bool keep);
    bool GroundUnderCursor(Vec3& point);
    void RemovePlacement(u32 section, int placement, u32 hash);

    // The sidebar: Scene (what has been put in the world) and Library (the
    // catalogue), sharing one search box.
    void ShowSideTab(int tab);
    void FocusPlacement(size_t section, size_t index);
    void FocusMine(size_t index);
    // The city's named zones: a quick trip to one, or to a whole district,
    // from the scene tree or Camera > Travel To.
    void TravelToZone(size_t index);
    void TravelToDistrict(size_t index);
    void TravelToBounds(const Vec2& minimum, const Vec2& maximum, const std::wstring& name);
    void BuildTravelMenu();
    size_t SceneSignature() const;
    void EditSelectedPlacement();
    void DeleteSelectedPlacement();
    // Copies the selected objects -- placed ones or the map's own -- and puts
    // copies at the view centre, keeping their layout.
    void CopySelection();
    void PasteClipboard();
    void SnapSelectedPlacement();

    // The ground under the middle of the view, onto the collision when there
    // is some there. False when the view looks at nothing near.
    bool ViewCentrePoint(Vec3& point);

    // The extension that owns a catalogue or edits row, or null.
    EditorExtension* ExtensionOf(const void* owner) const;
    bool SavePlacements();
    bool HasUnsavedPlacements() const;
    // Asks what to do with unsaved placements. False means stay.
    bool ConfirmUnsavedPlacements();

    // A viewpoint kept between runs, so coming back to the spot being worked
    // on does not mean flying there again after every rebuild.
    void SaveCameraPosition();
    void RestoreCameraPosition();

    // Starts the game over the viewport, the car on the ground under the
    // camera; while it runs, the button switches between the game and the map.
    void LaunchGame();
    void WriteLaunchPose();
    void UpdateGameView();         // keeps the game over the viewport, notices it quit
    void RestoreFastBoot(bool onlyWhenRead = false);

    // Reads an edited OBJ back into the game's files, after backing them up.
    void ImportEditedObjects();
    void ImportEditedObjectsImpl();
    // The import itself, once the OBJ is known. `quick` is the Blender round
    // trip: the camera stays where it is and success goes to the status bar
    // rather than a dialog, since it happens on every save.
    // `newOrigin`, when given, is where an OBJ without a manifest goes: every
    // shape in it is a new object.
    void ImportObj(const std::wstring& path, bool quick, const Vec3* newOrigin = nullptr);
    void ShowEditedMeshes();

    // Objects > Edit in Blender: exports the selection to a folder of its own,
    // opens it in Blender, and imports it again each time Blender saves.
    void EditInBlender();
    void PollBlender();
    // New objects: modelled in Blender at the view centre, or read from an OBJ
    // and stood there. Each becomes a model of its own, placed in the world.
    void NewObjectInBlender();
    void NewObjectFromObj();
    // Ticks off objects made in the editor and takes them out of the region.
    void DeleteMyObjects();
    void CloseRegion();

    void LoadAllSections();
    void PopulateSectionTree();
    void OnSectionSelected();


    void SetRenderMode(RenderMode mode);
    void UpdateMenuChecks();
    void UpdateStatusBar();
    void SetStatusText(int part, const std::wstring& text);

    // The frame drawn with ImGui (ui/MainWindowShell.cpp): the native
    // controls stay, hidden, as what everything writes to.
    bool shell_ = false;
    // Where the 3D view is, on screen, between the docked panels: the game
    // opens there, the panels and bars stay usable round it.
    RECT gameArea_{};
    void DrawShellBars();
    void DrawShellPanels();
    void DrawShellMenu(HMENU menu, bool top);
    void DrawShellTree(HTREEITEM item, int depth);
    void DrawShellLibrary();
    void DrawShellEdits();
    // Object / Edit mode, floating in a corner of the view.
    void DrawViewModes();
    class MeshTools* meshTools_ = nullptr;   // one of extensions_
    class ZoneTools* zoneTools_ = nullptr;   // one of extensions_
    // The Explorer's Online tab: Sketchfab models (ui/OnlineLibrary.h).
    std::unique_ptr<OnlineLibrary> online_;
    void PlaceOnlineModel();

    // Night lighting baked into the selected objects' vertex colours
    // (io/LightBake.h): a dialog drawn with the panels, a preview on screen,
    // and the write, which goes the way an edit from Blender comes back.
    void DrawBakeDialog();
    void ApplyBake(bool preview);
    void RestoreBake();
    void WriteBake();
    bool bakeOpen_ = false;
    bool bakeNatives_ = false;
    bool bakeMeasured_ = false;
    float bakeAmbient_ = 1.0f, bakeLamps_ = 1.0f, bakeRadius_ = 24.0f;
    std::vector<PickResult> bakeTargets_;
    std::map<u32, std::vector<u32>> bakeOriginal_;   // model hash -> colours before the preview
    static constexpr UINT kBakeWriteMessage = WM_APP + 0x54;
    void UpdateShellDrag();
    void HideNativeShell();
    void* ShellThumbnail(int image);
    void ForgetShellThumbnails();
    HIMAGELIST thumbList_ = nullptr;
    std::unordered_map<int, void*> thumbTextures_;
    bool shellDragging_ = false;
    int  shellDragItem_ = -1;
    int  shownTab_ = -1;
    static constexpr UINT kShellTreeActivate = WM_APP + 0x51;
    static constexpr UINT kStopGameMessage = WM_APP + 0x55;
    // View > Sky and View > Animations (added to the View menu at start).
    static constexpr int kViewSkyCommand = 44601, kViewAnimationsCommand = 44602;
    void StopGame();
    static constexpr UINT kShellLibraryActivate = WM_APP + 0x52;

    std::wstring LoadStoredGameFolder() const;
    bool RefuseReadOnlyWorld();
    std::wstring CameraValueName() const;
    void         StoreGameFolder(const std::wstring& folder) const;
    DWORD        StoredSetupVersion() const;
    void         StoreSetupVersion(DWORD version) const;

    HINSTANCE instance_     = nullptr;
    HWND      window_       = nullptr;
    HWND      regionCombo_  = nullptr;
    HWND      sectionTree_  = nullptr;
    HWND      statusBar_    = nullptr;
    HWND      toolbar_      = nullptr;
    HIMAGELIST toolbarImages_ = nullptr;
    HMENU     menu_         = nullptr;
    HACCEL    accelerators_ = nullptr;
    HFONT     uiFont_       = nullptr;

    Viewport      viewport_;
    EditsPanel    edits_;
    PropertiesPanel properties_;
    TextureBrowser textures_;
    OverlayBrowser overlays_;
    std::unique_ptr<nfs::World> world_;
    // What this build adds to the editor; see CreateEditorExtensions.
    std::vector<std::unique_ptr<EditorExtension>> extensions_;
    class CollisionTools* collisionTools_ = nullptr;   // one of extensions_
    bool rebuildingTree_ = false;       // selection changes while rebuilding are not clicks

    std::wstring gameFolder_;
    std::wstring tracksFolder_;
    // Where edited meshes live. Nothing is written into the game's own files;
    // an edit is a file here that both the editor and the ASI apply.
    std::wstring modelsFolder_;
    std::wstring worldName_;            // the world in modelsFolder_

    // Panel sizes, in pixels: the left panel's width, the properties' width
    // and the edits' height, each including its padding.
    int leftWidth_    = 260;
    int rightWidth_   = 250;
    int bottomHeight_ = 190;
    Splitter dragging_ = Splitter::None;
    int dragOffset_ = 0;                // from the pointer to the gap's edge
    RECT viewRect_{};                   // the viewport, as last laid out
    int contentBottom_ = 0;             // above the status bar

    double lastStatusUpdate_ = 0.0;

    EmbeddedGame game_;
    // FRSFastTest was switched on for the launch in flight, at this tick.
    bool  fastBootRestore_ = false;
    DWORD fastBootSince_ = 0;

    std::vector<nfs::PlacedInstance> clipboard_;

    BlenderLink blender_;
    ModelThumbnails thumbnails_;          // pictures for the Place Object catalogue
    // Models whose picture is not on disk yet, drawn in the background after a
    // region loads so the catalogue opens on finished pictures.
    std::vector<const nfs::SolidObject*> thumbsToWarm_;
    size_t thumbsWarmed_ = 0;
    void QueueThumbnailWarmUp();
    void WarmThumbnails(DWORD budgetMs);

    HWND sideTabs_ = nullptr, sideSearch_ = nullptr;
    HWND libShelf_ = nullptr, libUsed_ = nullptr, libList_ = nullptr;
    ModelCatalog catalog_;                // the Library tab
    int sideTab_ = 0;                     // 0 scene, 1 library
    std::vector<u32> mineHashes_;         // the scene tree's "My objects", in order
    std::vector<nfs::CityZone> zones_;    // read with the region
    size_t sceneSignature_ = 0;           // what the scene tree was built from

    bool  libDragging_ = false;              // a library drag is under way
    u32   dragHash_ = 0;
    bool  dragPlaced_ = false;            // its placement exists yet
    u32   dragSection_ = 0;
    int   dragPlacement_ = -1;
    bool  dragAddedImport_ = false;       // the drag put the model on the copy list
    EditorExtension* dragExtension_ = nullptr;   // the drag is one of its catalogue rows
    double scenePoll_ = 0.0;
    double      blenderPoll_ = 0.0;
};
