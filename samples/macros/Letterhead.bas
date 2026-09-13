' Letterhead: a heading, the date and a greeting at the top of the
' document, in Word42 Basic.  Tools > Macro > Macros, Run.
Sub Letterhead()
    Selection.HomeKey Unit:=wdStory
    Selection.Style = wdStyleTitle
    Selection.TypeText Text:="Word42"
    Selection.TypeParagraph
    Selection.Style = wdStyleNormal
    Selection.ParagraphFormat.Alignment = wdAlignParagraphRight
    Selection.TypeText Text:=Date
    Selection.TypeParagraph
    Selection.ParagraphFormat.Alignment = wdAlignParagraphLeft
    Selection.TypeParagraph
    Selection.TypeText Text:="Dear " & InputBox("Who is the letter to?", "Letterhead", "Sir or Madam") & ","
    Selection.TypeParagraph
End Sub
