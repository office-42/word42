' WordReport: what the document comes to, in a message box, and every
' "teh" put right.  Two Subs in one file: the Macros box lists both.
Sub Report()
    Dim words As Integer, paragraphs As Integer
    words = ActiveDocument.Words.Count
    paragraphs = ActiveDocument.Paragraphs.Count
    MsgBox ActiveDocument.Name & " has " & words & " words in " & paragraphs & " paragraphs" _
        & " on " & ActiveDocument.ComputeStatistics(wdStatisticPages) & " page(s).", vbInformation, "Word Report"
End Sub

Sub FixTypos()
    Dim n As Integer
    n = 0
    Selection.HomeKey Unit:=wdStory
    With Selection.Find
        .ClearFormatting
        .Text = "teh"
        .MatchCase = False
        .MatchWholeWord = True
    End With
    Do While Selection.Find.Execute
        Selection.Text = "the"
        n = n + 1
        If n > 1000 Then Exit Do
    Loop
    Application.StatusBar = n & " typo(s) fixed"
End Sub
