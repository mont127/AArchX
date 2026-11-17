' a VBScript loop: arithmetic, string building, an array and a dictionary-like lookup by Select Case
Dim n, r, i, acc, s, a(999), t0
n = 60
If WScript.Arguments.Count > 0 Then n = CLng(WScript.Arguments(0))
t0 = Timer
acc = 0
For r = 1 To n
    For i = 0 To 999
        a(i) = (i * 7 + r) Mod 1013
    Next
    s = ""
    For i = 0 To 999
        Select Case a(i) Mod 4
            Case 0: acc = acc + a(i)
            Case 1: acc = acc - a(i) \ 3
            Case 2: acc = acc + Len(CStr(a(i)))
            Case Else: acc = acc + Sqr(a(i))
        End Select
        If i Mod 50 = 0 Then s = s & Chr(65 + i Mod 26) & Mid("abcdefgh", 1 + i Mod 8, 1)
    Next
    acc = acc + Len(s) + InStr(s, "Aa") + Len(UCase(s) & LCase(s))
Next
WScript.Echo CStr(Int(acc)) & " " & CStr(Int((Timer - t0) * 1000)) & " ms"
