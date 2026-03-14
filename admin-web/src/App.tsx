import { BrowserRouter, Routes, Route } from 'react-router-dom'
import Layout from './components/Layout'
import Overview from './pages/Overview'
import Conversations from './pages/Conversations'
import ConversationDetail from './pages/ConversationDetail'

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<Layout />}>
          <Route index element={<Overview />} />
          <Route path="conversations" element={<Conversations />} />
          <Route path="conversations/:id" element={<ConversationDetail />} />
        </Route>
      </Routes>
    </BrowserRouter>
  )
}
