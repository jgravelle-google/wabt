(module
	(import "env" "puts" (func $puts (param i32) (result i32)))
	(memory 1)
	(data (i32.const 8) "hello world!\n")
	(data (i32.const 32) "world says hi\n")
	(export "main" (func $main))
	(func $main (result i32)
		(drop
			(call $puts
				(i32.const 8)
			)
		)
;;		(call $doPrint
;;			(i32.const 32)
;;		)
		(return
			(i32.const 0)
		)
	)
;;	(func $doPrint (param i32)
;;		(drop
;;			(call $puts
;;				(get_local 0)
;;			)
;;		)
;;	)
;;	(func $getConstFloat (result f32)
;;		(return
;;			(f32.const 100)
;;		)
;;	)
)
