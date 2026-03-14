import { useEffect, useState } from 'react'
import { useNavigate, useSearchParams } from 'react-router-dom'
import { api } from '../api/admin'
import type { ConversationSummary } from '../api/admin'

const PAGE_SIZE = 20

function fmt(v: number | null) {
  return v == null ? 'N/A' : `${v} ms`
}

function formatTime(iso: string) {
  return new Date(iso).toLocaleString('zh-CN', {
    month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

function truncate(s: string, n = 40) {
  return s.length > n ? s.slice(0, n) + '…' : s
}

export default function Conversations() {
  const [searchParams, setSearchParams] = useSearchParams()
  const page = Number(searchParams.get('page') ?? '1')
  const navigate = useNavigate()

  const [items, setItems] = useState<ConversationSummary[] | null>(null)
  const [total, setTotal] = useState(0)
  const [error, setError] = useState(false)

  useEffect(() => {
    setItems(null)
    setError(false)
    api.getConversations(page, PAGE_SIZE)
      .then(r => { setItems(r.items); setTotal(r.total) })
      .catch(() => setError(true))
  }, [page])

  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE))

  return (
    <div className="max-w-5xl">
      <h1 className="text-xl font-semibold text-gray-800 mb-5">对话记录</h1>

      {error && (
        <p className="mb-4 text-sm text-red-500">获取数据失败，请刷新页面重试</p>
      )}

      <div className="bg-white rounded-lg shadow">
        <table className="w-full text-sm">
          <thead className="text-xs text-gray-400 uppercase bg-gray-50 border-b border-gray-100">
            <tr>
              <th className="px-5 py-3 text-left">时间</th>
              <th className="px-5 py-3 text-left">用户说</th>
              <th className="px-5 py-3 text-left">AI 回复</th>
              <th className="px-5 py-3 text-right">首包时间</th>
              <th className="px-5 py-3 text-right">总时间</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-gray-50">
            {items === null ? (
              [...Array(5)].map((_, i) => (
                <tr key={i}>
                  <td colSpan={5} className="px-5 py-3">
                    <div className="h-4 bg-gray-100 rounded animate-pulse" />
                  </td>
                </tr>
              ))
            ) : items.length === 0 ? (
              <tr>
                <td colSpan={5} className="px-5 py-8 text-center text-gray-400">
                  暂无对话记录
                </td>
              </tr>
            ) : (
              items.map(r => (
                <tr
                  key={r.id}
                  className="hover:bg-gray-50 cursor-pointer"
                  onClick={() => navigate(`/conversations/${r.id}`)}
                >
                  <td className="px-5 py-3 text-gray-500 whitespace-nowrap">{formatTime(r.created_at)}</td>
                  <td className="px-5 py-3 text-gray-800">{truncate(r.user_text)}</td>
                  <td className="px-5 py-3 text-gray-500">{truncate(r.ai_text)}</td>
                  <td className={`px-5 py-3 text-right font-mono ${(r.e2e_ttfa_ms ?? 0) > 1000 ? 'text-red-500' : 'text-gray-600'}`}>
                    {fmt(r.e2e_ttfa_ms)}
                  </td>
                  <td className="px-5 py-3 text-right font-mono text-gray-500">
                    {fmt(r.e2e_total_ms)}
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>

        {/* 分页 */}
        <div className="flex items-center justify-between px-5 py-3 border-t border-gray-100 text-sm text-gray-500">
          <span>共 {total} 条</span>
          <div className="flex items-center gap-3">
            <button
              disabled={page <= 1}
              onClick={() => setSearchParams({ page: String(page - 1) })}
              className="px-3 py-1 rounded border border-gray-200 disabled:opacity-40 hover:bg-gray-50"
            >
              ← 上一页
            </button>
            <span>第 {page} / {totalPages} 页</span>
            <button
              disabled={page >= totalPages}
              onClick={() => setSearchParams({ page: String(page + 1) })}
              className="px-3 py-1 rounded border border-gray-200 disabled:opacity-40 hover:bg-gray-50"
            >
              下一页 →
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}
