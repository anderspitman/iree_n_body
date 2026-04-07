#map = affine_map<(d0, d1) -> (d0, d1)>

func.func @step(%state: tensor<?x5xf32>, %gravity: f32, %softening: f32, %dt: f32) -> tensor<?x5xf32> {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c2 = arith.constant 2 : index
  %c3 = arith.constant 3 : index
  %c4 = arith.constant 4 : index
  %body_count = tensor.dim %state, %c0 : tensor<?x5xf32>
  %f0 = arith.constant 0.0 : f32
  %empty = tensor.empty(%body_count) : tensor<?x5xf32>
  %out = linalg.generic {
    indexing_maps = [#map, #map],
    iterator_types = ["parallel", "parallel"]
  } ins(%state : tensor<?x5xf32>) outs(%empty : tensor<?x5xf32>) {
  ^bb0(%in: f32, %outv: f32):
    %i = linalg.index 0 : index
    %field = linalg.index 1 : index
    %x = tensor.extract %state[%i, %c0] : tensor<?x5xf32>
    %y = tensor.extract %state[%i, %c1] : tensor<?x5xf32>
    %vx = tensor.extract %state[%i, %c2] : tensor<?x5xf32>
    %vy = tensor.extract %state[%i, %c3] : tensor<?x5xf32>
    %mass = tensor.extract %state[%i, %c4] : tensor<?x5xf32>
    %pair:2 = scf.for %j = %c0 to %body_count step %c1 iter_args(%ax = %f0, %ay = %f0) -> (f32, f32) {
      %same_body = arith.cmpi eq, %i, %j : index
      %next_pair:2 = scf.if %same_body -> (f32, f32) {
        scf.yield %ax, %ay : f32, f32
      } else {
        %xj = tensor.extract %state[%j, %c0] : tensor<?x5xf32>
        %yj = tensor.extract %state[%j, %c1] : tensor<?x5xf32>
        %massj = tensor.extract %state[%j, %c4] : tensor<?x5xf32>
        %dx = arith.subf %xj, %x : f32
        %dy = arith.subf %yj, %y : f32
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
    %dvx = arith.mulf %pair#0, %dt : f32
    %dvy = arith.mulf %pair#1, %dt : f32
    %next_vx = arith.addf %vx, %dvx : f32
    %next_vy = arith.addf %vy, %dvy : f32
    %dx = arith.mulf %next_vx, %dt : f32
    %dy = arith.mulf %next_vy, %dt : f32
    %next_x = arith.addf %x, %dx : f32
    %next_y = arith.addf %y, %dy : f32
    %is_x = arith.cmpi eq, %field, %c0 : index
    %value = scf.if %is_x -> (f32) {
      scf.yield %next_x : f32
    } else {
      %is_y = arith.cmpi eq, %field, %c1 : index
      %value_y = scf.if %is_y -> (f32) {
        scf.yield %next_y : f32
      } else {
        %is_vx = arith.cmpi eq, %field, %c2 : index
        %value_vx = scf.if %is_vx -> (f32) {
          scf.yield %next_vx : f32
        } else {
          %is_vy = arith.cmpi eq, %field, %c3 : index
          %value_vy = scf.if %is_vy -> (f32) {
            scf.yield %next_vy : f32
          } else {
            scf.yield %mass : f32
          }
          scf.yield %value_vy : f32
        }
        scf.yield %value_vx : f32
      }
      scf.yield %value_y : f32
    }
    linalg.yield %value : f32
  } -> tensor<?x5xf32>
  return %out : tensor<?x5xf32>
}
