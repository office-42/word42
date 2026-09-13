' Emphasis: the selection made bold and red, or the emphasis taken off
' again if it has it -- a toggle, the way a toolbar button works.
Sub Emphasis()
    If Selection.Font.Bold Then
        Selection.Font.Bold = False
        Selection.Font.Color = wdColorAutomatic
    Else
        Selection.Font.Bold = True
        Selection.Font.Color = wdColorRed
    End If
End Sub
