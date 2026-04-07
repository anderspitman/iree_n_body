func.func @step(%state: tensor<8x5xf32>, %gravity: f32, %softening: f32, %dt: f32) -> tensor<8x5xf32> {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c2 = arith.constant 2 : index
  %c3 = arith.constant 3 : index
  %c4 = arith.constant 4 : index
  %c8 = arith.constant 8 : index
  %f0 = arith.constant 0.0 : f32
  %empty_acc = tensor.empty() : tensor<8x2xf32>
  %acc = scf.for %i = %c0 to %c8 step %c1 iter_args(%acc_iter = %empty_acc) -> (tensor<8x2xf32>) {
    %xi = tensor.extract %state[%i, %c0] : tensor<8x5xf32>
    %yi = tensor.extract %state[%i, %c1] : tensor<8x5xf32>
    %pair:2 = scf.for %j = %c0 to %c8 step %c1 iter_args(%ax = %f0, %ay = %f0) -> (f32, f32) {
      %same_body = arith.cmpi eq, %i, %j : index
      %next_pair:2 = scf.if %same_body -> (f32, f32) {
        scf.yield %ax, %ay : f32, f32
      } else {
        %xj = tensor.extract %state[%j, %c0] : tensor<8x5xf32>
        %yj = tensor.extract %state[%j, %c1] : tensor<8x5xf32>
        %massj = tensor.extract %state[%j, %c4] : tensor<8x5xf32>
        %dx = arith.subf %xj, %xi : f32
        %dy = arith.subf %yj, %yi : f32
        %dx_sq = arith.mulf %dx, %dx : f32
        %dy_sq = arith.mulf %dy, %dy : f32
        %distance_sq_base = arith.addf %dx_sq, %dy_sq : f32
        %distance_sq = arith.addf %distance_sq_base, %softening : f32
        %distance = math.sqrt %distance_sq : f32
        %denominator = arith.mulf %distance_sq, %distance : f32
        %scale_force = arith.divf %gravity, %denominator : f32
        %mass_force = arith.mulf %scale_force, %massj : f32
        %dax = arith.mulf %mass_force, %dx : f32
        %day = arith.mulf %mass_force, %dy : f32
        %next_ax = arith.addf %ax, %dax : f32
        %next_ay = arith.addf %ay, %day : f32
        scf.yield %next_ax, %next_ay : f32, f32
      }
      scf.yield %next_pair#0, %next_pair#1 : f32, f32
    }
    %acc_x = tensor.insert %pair#0 into %acc_iter[%i, %c0] : tensor<8x2xf32>
    %acc_xy = tensor.insert %pair#1 into %acc_x[%i, %c1] : tensor<8x2xf32>
    scf.yield %acc_xy : tensor<8x2xf32>
  }
  %empty_out = tensor.empty() : tensor<8x5xf32>
  %next_state = scf.for %i = %c0 to %c8 step %c1 iter_args(%state_iter = %empty_out) -> (tensor<8x5xf32>) {
    %x = tensor.extract %state[%i, %c0] : tensor<8x5xf32>
    %y = tensor.extract %state[%i, %c1] : tensor<8x5xf32>
    %vx = tensor.extract %state[%i, %c2] : tensor<8x5xf32>
    %vy = tensor.extract %state[%i, %c3] : tensor<8x5xf32>
    %mass = tensor.extract %state[%i, %c4] : tensor<8x5xf32>
    %ax = tensor.extract %acc[%i, %c0] : tensor<8x2xf32>
    %ay = tensor.extract %acc[%i, %c1] : tensor<8x2xf32>
    %dvx = arith.mulf %ax, %dt : f32
    %dvy = arith.mulf %ay, %dt : f32
    %next_vx = arith.addf %vx, %dvx : f32
    %next_vy = arith.addf %vy, %dvy : f32
    %dx = arith.mulf %next_vx, %dt : f32
    %dy = arith.mulf %next_vy, %dt : f32
    %next_x = arith.addf %x, %dx : f32
    %next_y = arith.addf %y, %dy : f32
    %with_x = tensor.insert %next_x into %state_iter[%i, %c0] : tensor<8x5xf32>
    %with_xy = tensor.insert %next_y into %with_x[%i, %c1] : tensor<8x5xf32>
    %with_xyvx = tensor.insert %next_vx into %with_xy[%i, %c2] : tensor<8x5xf32>
    %with_xyvxvy = tensor.insert %next_vy into %with_xyvx[%i, %c3] : tensor<8x5xf32>
    %with_all = tensor.insert %mass into %with_xyvxvy[%i, %c4] : tensor<8x5xf32>
    scf.yield %with_all : tensor<8x5xf32>
  }
  return %next_state : tensor<8x5xf32>
}
