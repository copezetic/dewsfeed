# Per-line running paren depth for JS, aware of ' " ` strings, // and /* */ comments.
# Prints lines where depth CHANGES so unmatched opens are easy to spot.
BEGIN{ depth=0; inBlock=0; mode="" }  # mode: "", sq, dq, bt
{
  line=$0; n=length(line); i=1; startDepth=depth
  while(i<=n){
    c=substr(line,i,1); two=substr(line,i,2)
    if(inBlock){ if(two=="*/"){inBlock=0;i+=2} else i++; continue }
    if(mode=="sq"){ if(c=="\\")i+=2; else {if(c=="'")mode=""; i++}; continue }
    if(mode=="dq"){ if(c=="\\")i+=2; else {if(c=="\"")mode=""; i++}; continue }
    if(mode=="bt"){
      if(c=="\\"){i+=2;continue}
      if(c=="`"){mode="";i++;continue}
      if(two=="${"){mode="";i+=2;btPend++;continue}  # code island; crude: treat rest as code until }
      i++; continue
    }
    if(two=="/*"){inBlock=1;i+=2;continue}
    if(two=="//"){break}
    if(c=="'"){mode="sq";i++;continue}
    if(c=="\""){mode="dq";i++;continue}
    if(c=="`"){mode="bt";i++;continue}
    if(c=="(")depth++
    else if(c==")")depth--
    i++
  }
  if(depth!=startDepth) printf "%5d  depth %+d -> %d : %s\n", NR, depth-startDepth, depth, substr(line,1,90)
}
END{ printf "FINAL DEPTH: %d\n", depth }
