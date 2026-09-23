#pragma once
class MainWindowController {
   private:
    bool m_showLeftSidebar = true;
    bool m_showBottomBar = true;

   public:
    MainWindowController() = default;
    ~MainWindowController() = default;

    bool isLeftSidebarVisible() const { return m_showLeftSidebar; }
    bool isBottomBarVisible() const { return m_showBottomBar; }

    void toggleLeftSidebar() { m_showLeftSidebar = !m_showLeftSidebar; }
    void toggleBottomBar() { m_showBottomBar = !m_showBottomBar; }

    void setLeftSidebarVisible(bool visible) { m_showLeftSidebar = visible; }
    void setBottomBarVisible(bool visible) { m_showBottomBar = visible; }
};
