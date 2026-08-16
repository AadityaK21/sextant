import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'

import App from './App'
import './index.css'

// staleTime of 30s because the underlying data only changes when someone runs
// `sextant resolve`, which is a deliberate act rather than a stream. Refetching
// on every window focus would make the cost numbers on screen jitter for no
// reason, and those numbers are the point.
const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 30_000,
      refetchOnWindowFocus: false,
      retry: (failureCount, error) => {
        // Do not retry a 4xx: the request will not become valid by repeating.
        const status = (error as { status?: number }).status
        if (status !== undefined && status >= 400 && status < 500) return false
        return failureCount < 2
      },
    },
  },
})

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <App />
    </QueryClientProvider>
  </StrictMode>,
)
