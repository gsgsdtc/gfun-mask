import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api } from '../api/admin'
import type { StatsResponse, ConversationSummary } from '../api/admin'

function StatCard({ label, value }: { label: string; value: string }) {
  return (
    <div className="bg-white rounded-lg shadow p-5 flex flex-col gap-1">
      <p className="text-xs text-gray-500 uppercase tracking-wider">{label}</p>
      <p className="text-2xl font-bold text-gray-900">{value}</p>
    </div>
  )
}

function fmt(v: number | null, unit = 'ms') {
  return v == null ? 'N/A' : `${v} ${unit}`
}

function formatTime(iso: string) {
  return new Date(iso).toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function truncate(s: string, n = 30) {
  return s.length > n ? s.slice(0, n) + '…' : s
}

export default function Overview() {
  const [stats, setStats] = useState<StatsResponse | null>(null)
  const [error, setError] = useState(false)

  useEffect(() => {
    api.getStats()
      .then(setStats)
      .catch(() => setError(true))
  }, [])

  const cards = [
    { label: '今日对话', value: stats ? String(stats.today_count) : '--' },
    { label: '平均整体首包', value: fmt(stats?.avg_e2e_ttfa_ms ?? null) },
    { label: '平均 ASR 耗时', value: fmt(stats?.avg_asr_total_ms ?? null) },
    { label: '平均 LLM TTFT', value: fmt(stats?.avg_llm_ttft_ms ?? null) },
  ]

  return (
    <div className="max-w-5xl">
      <h1 className="text-xl font-semibold text-gray-800 mb-5">概览</h1>

      {error && (
        <p className="mb-4 text-sm text-red-500">获取数据失败，请刷新页面重试</p>
      )}

      {/* 统计卡片 */}
      <div className="grid grid-cols-4 gap-4 mb-8">
        {cards.map(c => <StatCard key={c.label} {...c} />)}
      </div>

      {/* 最近对话 */}
      <div className="bg-white rounded-lg shadow">
        <div className="flex items-center justify-between px-5 py-3 border-b border-gray-100">
          <h2 className="font-medium text-gray-700">最近对话</h2>
          <Link to="/conversations" className="text-sm text-blue-500 hover:underline">
            查看全部 →
          </Link>
        </div>
        <RecentTable rows={stats?.recent ?? null} />
      </div>
    </div>
  )
}

function RecentTable({ rows }: { rows: ConversationSummary[] | null }) {
  if (rows === null) {
    return (
      <div className="px-5 py-4">
        <div className="animate-pulse space-y-3">
          {[...Array(3)].map((_, i) => (
            <div key={i} className="h-4 bg-gray-100 rounded w-full" />
          ))}
        </div>
      </div>
    )
  }
  if (rows.length === 0) {
    return <p className="px-5 py-4 text-sm text-gray-400">暂无对话记录</p>
  }
  return (
    <table className="w-full text-sm">
      <thead className="text-xs text-gray-400 uppercase bg-gray-50">
        <tr>
          <th className="px-5 py-2 text-left">时间</th>
          <th className="px-5 py-2 text-left">用户说</th>
          <th className="px-5 py-2 text-left">AI 回复</th>
          <th className="px-5 py-2 text-right">首包时间</th>
        </tr>
      </thead>
      <tbody className="divide-y divide-gray-50">
        {rows.map(r => (
          <tr
            key={r.id}
            className="hover:bg-gray-50 cursor-pointer"
            onClick={() => window.location.href = `/conversations/${r.id}`}
          >
            <td className="px-5 py-3 text-gray-500 whitespace-nowrap">{formatTime(r.created_at)}</td>
            <td className="px-5 py-3 text-gray-800">{truncate(r.user_text)}</td>
            <td className="px-5 py-3 text-gray-500">{truncate(r.ai_text)}</td>
            <td className={`px-5 py-3 text-right font-mono ${(r.e2e_ttfa_ms ?? 0) > 1000 ? 'text-red-500' : 'text-gray-600'}`}>
              {fmt(r.e2e_ttfa_ms)}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}
