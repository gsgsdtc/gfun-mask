const BASE = '/api/admin'

export interface StatsResponse {
  today_count: number
  avg_e2e_ttfa_ms: number | null
  avg_asr_total_ms: number | null
  avg_llm_ttft_ms: number | null
  avg_tts_ttfa_ms: number | null
  recent: ConversationSummary[]
}

export interface ConversationSummary {
  id: number
  created_at: string
  user_text: string
  ai_text: string
  e2e_ttfa_ms: number | null
}

export interface ConversationsResponse {
  total: number
  page: number
  size: number
  items: ConversationSummary[]
}

export interface ConversationDetail {
  id: number
  session_id: string
  created_at: string
  user_text: string
  ai_text: string
  asr_ttfa_ms: number | null
  asr_total_ms: number | null
  llm_ttft_ms: number | null
  llm_total_ms: number | null
  tts_ttfa_ms: number | null
  tts_total_ms: number | null
  e2e_ttfa_ms: number | null
}

async function get<T>(path: string): Promise<T> {
  const res = await fetch(BASE + path)
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

export const api = {
  getStats: () => get<StatsResponse>('/stats'),
  getConversations: (page = 1, size = 20) =>
    get<ConversationsResponse>(`/conversations?page=${page}&size=${size}`),
  getConversation: (id: number) =>
    get<ConversationDetail>(`/conversations/${id}`),
}
