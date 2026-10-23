interface LogoProps {
  size?: number
  title?: string
  className?: string
}

export function Logo({ size = 28, title, className }: LogoProps) {
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox="0 0 120 120"
      role={title ? 'img' : undefined}
      aria-hidden={title ? undefined : true}
      aria-label={title}
    >
      <path fill="currentColor" d="M14 22H106C106 46 88 60 60 60C32 60 14 46 14 22Z" />
      <path
        fill="none"
        stroke="currentColor"
        strokeWidth="6"
        strokeLinecap="round"
        strokeLinejoin="round"
        d="M14 78q11.5 -8 23 0t23 0t23 0t23 0"
      />
      <path
        fill="none"
        stroke="currentColor"
        strokeWidth="6"
        strokeLinecap="round"
        strokeLinejoin="round"
        d="M14 94q11.5 -8 23 0t23 0t23 0t23 0"
      />
    </svg>
  )
}
