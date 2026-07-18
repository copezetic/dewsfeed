BEGIN { inBlock=0; o=0; cc=0; ob=0; cb=0 }
{
  line = $0
  n = length(line)
  i = 1
  while (i <= n) {
    if (inBlock) {
      if (substr(line,i,2) == "*/") { inBlock=0; i+=2 } else { i++ }
      continue
    }
    two = substr(line,i,2)
    c   = substr(line,i,1)
    if (two == "/*") { inBlock=1; i+=2; continue }
    if (two == "//") { break }                       # rest of line is comment
    if (c == "\"") {                                  # skip string literal
      i++
      while (i <= n) {
        ch = substr(line,i,1)
        if (ch == "\\") { i+=2; continue }
        if (ch == "\"") { i++; break }
        i++
      }
      continue
    }
    if (c == "'") {                                   # skip char literal
      i++
      while (i <= n) {
        ch = substr(line,i,1)
        if (ch == "\\") { i+=2; continue }
        if (ch == "'") { i++; break }
        i++
      }
      continue
    }
    if (c == "(") o++
    else if (c == ")") cc++
    else if (c == "{") ob++
    else if (c == "}") cb++
    i++
  }
}
END {
  printf "parens: open=%d close=%d %s\n", o, cc, (o==cc ? "OK" : "MISMATCH " (o-cc))
  printf "braces: open=%d close=%d %s\n", ob, cb, (ob==cb ? "OK" : "MISMATCH " (ob-cb))
}
