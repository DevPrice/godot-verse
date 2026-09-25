using { /Godot.org/Godot }

# spec/values.md §4-5: char, char32, strings as []char.

values_strings_chars := class(object):

	CharBasics<public>():void =
		C:char = 'a'
		D:char = 'b'
		Print("ToString('a')={ToString(C)} interp={C}")
		Print("'a'='a': {if (C = 'a') then "yes" else "no"}")
		Print("'a'<>'b': {if (C <> D) then "yes" else "no"}")

	Char32Basics<public>():void =
		E := 0u00e9
		Print("char32 ToString={ToString(E)} interp={E}")

	StringLengthAndIndex<public>():void =
		S := "héllo"
		Print("Length={S.Length}")
		if (B := S[0]) then Print("S[0]={B}")
		if (X := "abc"[3]) then Print("abc[3] succeeds") else Print("abc[3] fails")
		Print("empty.Length={"".Length}")

	StringConcatAndEquality<public>():void =
		Print("ab+cd={"ab" + "cd"}")
		Print("ab=ab: {if ("ab" = "ab") then "yes" else "no"}")
		Print("ab<>abc: {if ("ab" <> "abc") then "yes" else "no"}")
		Print("array a,b = ab: {if (array{'a', 'b'} = "ab") then "yes" else "no"}")
		Built:string = array{'x', 'y'}
		Print("built from chars={Built}")

	StringFalseAsEmpty<public>():void =
		F:string = false
		Print("false-as-string Length={F.Length}")
		Print("false = empty string: {if (F = "") then "yes" else "no"}")

	StringInterpolation<public>():void =
		Print("char literal interp={'z'}!")
		Print("nested interp={"in{1}ner"}")
		Print("empty interp={}")

	CharVsString<public>():void =
		Print("'a' = \"a\" via comparable: {if ('a' = "a") then "yes" else "no"}")
