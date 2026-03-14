import { useEffect, useState } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { api } from '../api/admin'
import type { ConversationDetail } from '../api/admin'

function fmt(v: number | null) {
  return v == null ? 'N/A' : `${v} ms`
}

function formatDateTime(iso: string) {
  return new Date(iso).toLocaleString('zh-CN')
}

// 时间轴条：显示各阶段首包时间在 e2e 中的占比
function LatencyTimeline({ detail }: { detail: ConversationDetail }) {
  const e2e = detail.e2e_ttfa_ms
  if (!e2e || e2e === 0) return null

  const segments = [
    { label: 'ASR', value: detail.asr_ttfa_ms, color: 'bg-blue-400', start: 0 },
    { label: 'LLM', value: detail.llm_ttft_ms, color: 'bg-violet-400', start: detail.asr_ttfa_ms ?? 0 },
    { label: 'TTS', value: detail.tts_ttfa_ms, color: 'bg-emerald-400', start: (detail.asr_ttfa_ms ?? 0) + (detail.llm_ttft_ms ?? 0) },
  ]

  return (
    <div className="space-y-3">
      {segments.map(seg => {
        const width = seg.value ? (seg.value / e2e) * 100 : 0
        const offset = (seg.start / e2e) * 100
        return (
          <div key={seg.label} className="flex items-center gap-3">
            <span className="w-8 text-xs text-gray-500 shrink-0">{seg.label}</span>
            <div className="flex-1 h-5 bg-gray-100 rounded relative overflow-hidden">
              <div
                className={`absolute h-full ${seg.color} rounded`}
                style={{ left: `${offset}%`, width: `${width}%` }}
              />
            </div>
            <span className="w-16 text-right text-xs font-mono text-gray-600 shrink-0">
              {fmt(seg.value)}
            </span>
          </div>
        )
      })}
    </div>
  )
}

export default function ConversationDetailPage() {
  const { id } = useParams<{ id: string }>()
  const navigate = useNavigate()
  const [detail, setDetail] = useState<ConversationDetail | null>(null)
  const [notFound, setNotFound] = useState(false)
  const [error, setError] = useState(false)

  useEffect(() => {
    if (!id) return
    api.getConversation(Number(id))
      .then(setDetail)
      .catch(e => {
        if (e.message?.includes('404')) setNotFound(true)
        else setError(true)
      })
  }, [id])

  if (notFound) return (
    <div className="max-w-3xl">
      <button onClick={() => navigate(-1)} className="text-sm text-blue-500 hover:underline mb-4 flex items-center gap-1">
        ← 返回列表
      </button>
      <p className="text-gray-400">未找到该对话记录（id={id}）</p>
    </div>
  )

  if (error) return (
    <div className="max-w-3xl">
      <button onClick={() => navigate(-1)} className="text-sm text-blue-500 hover:underline mb-4 flex items-center gap-1">
        ← 返回列表
      </button>
      <p className="text-red-500 text-sm">获取数据失败，请刷新重试</p>
    </div>
  )

  return (
    <div className="max-w-3xl">
      {/* 标题栏 */}
      <div className="flex items-center justify-between mb-5">
        <button
          onClick={() => navigate('/conversations')}
          className="text-sm text-blue-500 hover:underline flex items-center gap-1"
        >
          ← 返回列表
        </button>
        {detail && (
          <span className="text-sm text-gray-400">
            对话详情 #{detail.id} &nbsp;·&nbsp; {formatDateTime(detail.created_at)}
          </span>
        )}
      </div>

      {!detail ? (
        <div className="space-y-4 animate-pulse">
          <div className="h-24 bg-gray-100 rounded-lg" />
          <div className="h-40 bg-gray-100 rounded-lg" />
        </div>
      ) : (
        <div className="space-y-5">
          {/* 对话气泡 */}
          <div className="bg-white rounded-lg shadow p-5 space-y-4">
            <h2 className="font-medium text-gray-700">对话内容</h2>
            <div className="space-y-3">
              <div className="flex gap-3">
                <span className="text-xl shrink-0">👤</span>
                <div className="bg-blue-50 rounded-lg px-4 py-3 text-sm text-gray-800 flex-1">
                  {detail.user_text || <span className="text-gray-400">（空）</span>}
                </div>
              </div>
              <div className="flex gap-3">
                <span className="text-xl shrink-0">🤖</span>
                <div className="bg-gray-50 rounded-lg px-4 py-3 text-sm text-gray-800 flex-1">
                  {detail.ai_text || <span className="text-gray-400">（空）</span>}
                </div>
              </div>
            </div>
          </div>

          {/* 耗时分解 */}
          <div className="bg-white rounded-lg shadow p-5">
            <h2 className="font-medium text-gray-700 mb-4">耗时分解</h2>
            <table className="w-full text-sm">
              <thead className="text-xs text-gray-400 uppercase">
                <tr>
                  <th className="text-left pb-2">环节</th>
                  <th className="text-right pb-2">首包时间</th>
                  <th className="text-right pb-2">总计时间</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-gray-50">
                {[
                  { label: 'ASR', ttfa: detail.asr_ttfa_ms, total: detail.asr_total_ms },
                  { label: 'LLM', ttfa: detail.llm_ttft_ms, total: detail.llm_total_ms },
                  { label: 'TTS', ttfa: detail.tts_ttfa_ms, total: detail.tts_total_ms },
                ].map(row => (
                  <tr key={row.label}>
                    <td className="py-2 text-gray-600 font-medium">{row.label}</td>
                    <td className="py-2 text-right font-mono text-gray-700">{fmt(row.ttfa)}</td>
                    <td className="py-2 text-right font-mono text-gray-500">{fmt(row.total)}</td>
                  </tr>
                ))}
                <tr className="border-t-2 border-gray-200">
                  <td className="py-2 font-semibold text-gray-800">整体首包</td>
                  <td className="py-2 text-right font-mono font-bold text-gray-900" colSpan={2}>
                    {fmt(detail.e2e_ttfa_ms)}
                  </td>
                </tr>
              </tbody>
            </table>
          </div>

          {/* 时间轴 */}
          {detail.e2e_ttfa_ms && (
            <div className="bg-white rounded-lg shadow p-5">
              <h2 className="font-medium text-gray-700 mb-4">首包时间轴</h2>
              <LatencyTimeline detail={detail} />
              <p className="mt-3 text-xs text-gray-400">
                横轴总宽度 = 整体首包时间（{detail.e2e_ttfa_ms} ms），色块宽度 = 各阶段占比
              </p>
            </div>
          )}
        </div>
      )}
    </div>
  )
}
