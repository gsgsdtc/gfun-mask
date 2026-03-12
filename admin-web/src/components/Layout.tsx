import { NavLink, Outlet } from 'react-router-dom'

const NAV = [
  { to: '/', label: '📊 概览', end: true },
  { to: '/conversations', label: '💬 对话记录' },
]

export default function Layout() {
  return (
    <div className="min-h-screen flex flex-col">
      {/* 顶部导航 */}
      <header className="bg-gray-900 text-white h-12 flex items-center px-6 shrink-0">
        <span className="font-semibold text-base tracking-wide">🎙 VoiceMask Admin</span>
      </header>

      <div className="flex flex-1">
        {/* 侧边栏 */}
        <aside className="w-48 bg-gray-800 text-gray-300 shrink-0 flex flex-col py-4">
          <nav className="flex flex-col gap-1 px-2">
            {NAV.map(({ to, label, end }) => (
              <NavLink
                key={to}
                to={to}
                end={end}
                className={({ isActive }) =>
                  `px-3 py-2 rounded text-sm transition-colors ${
                    isActive
                      ? 'bg-gray-700 text-white font-medium'
                      : 'hover:bg-gray-700 hover:text-white'
                  }`
                }
              >
                {label}
              </NavLink>
            ))}
          </nav>
          <div className="mt-4 mx-4 border-t border-gray-700" />
          <p className="px-5 mt-4 text-xs text-gray-500">（更多功能待扩展）</p>
        </aside>

        {/* 主内容 */}
        <main className="flex-1 p-6 overflow-auto">
          <Outlet />
        </main>
      </div>
    </div>
  )
}
